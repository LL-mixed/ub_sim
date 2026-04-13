#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
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

#define DP_PORT 18555
#define WAIT_IFACE_MS 90000
#define RX_TIMEOUT_MS 300
#define RUN_TIMEOUT_MS 30000

static volatile sig_atomic_t g_alarm_fired;
static unsigned int g_rx_debug_drops;
static bool g_log_peer_rx = true;

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

static unsigned int cmdline_get_u32_default(const char *key, unsigned int default_value)
{
    char buf[64];
    char *end = NULL;
    unsigned long parsed;

    if (!cmdline_get_value(key, buf, sizeof(buf))) {
        return default_value;
    }

    errno = 0;
    parsed = strtoul(buf, &end, 10);
    if (errno != 0 || end == buf || *end != '\0' || parsed > UINT_MAX) {
        fprintf(stderr, "[urma_dp] warn: bad %s=%s, use default=%u\n",
                key, buf, default_value);
        return default_value;
    }

    return (unsigned int)parsed;
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

static void dump_netdevs(void)
{
    FILE *fp;
    char line[512];

    fp = fopen("/proc/net/dev", "r");
    if (!fp) {
        return;
    }

    printf("[urma_dp] /proc/net/dev begin\n");
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t len = strlen(line);
        printf("[urma_dp] %s", line);
        if (len == 0 || line[len - 1] != '\n') {
            putchar('\n');
        }
    }
    printf("[urma_dp] /proc/net/dev end\n");
    fclose(fp);
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
    up = ((ifr.ifr_flags & IFF_UP) != 0) && ((ifr.ifr_flags & IFF_RUNNING) != 0);
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

    dump_netdevs();
    return false;
}

static bool set_ipv4_addr(const char *ifname, const char *addr_str)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[urma_dp] set_ipv4: socket failed: %s\n", strerror(errno));
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        fprintf(stderr, "[urma_dp] set_ipv4: inet_pton failed for %s\n", addr_str);
        close(fd);
        return false;
    }

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[urma_dp] set_ipv4: SIOCSIFADDR failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    /* set netmask 255.255.255.0 */
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[urma_dp] set_ipv4: SIOCSIFNETMASK failed: %s\n", strerror(errno));
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
        fprintf(stderr, "[urma_dp] warn: socket for SIOCSARP failed: %s\n", strerror(errno));
        return;
    }

    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[urma_dp] warn: SIOCSARP %s failed: %s\n",
                ifname, strerror(errno));
    } else {
        fprintf(stderr, "[urma_dp] static arp installed for peer on %s\n", ifname);
    }

    close(fd);
}

static int consume_probe_payload(const char *pkt, size_t pkt_len,
                                 const struct in_addr *src_addr,
                                 unsigned int rx_ifindex,
                                 unsigned int ifindex,
                                 const char *local_role,
                                 char *payload, size_t payload_len)
{
    const char *prefix = "urma-dp:";
    size_t prefix_len = strlen(prefix);
    const char *body;
    size_t body_len;
    const char *role_begin;
    const char *role_end;
    int payload_off = 0;
    bool local_role_msg = false;

    /* Some paths may not provide IP_PKTINFO, accept rx_ifindex=0. */
    if (rx_ifindex != 0 && rx_ifindex != ifindex) {
        if (g_rx_debug_drops < 16) {
            char src_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, src_addr, src_buf, sizeof(src_buf));
            fprintf(stderr, "[urma_dp] drop: ifindex mismatch src=%s rx_if=%u need=%u\n",
                    src_buf, rx_ifindex, ifindex);
            g_rx_debug_drops++;
        }
        return 0;
    }

    if (pkt_len >= prefix_len && strncmp(pkt, prefix, prefix_len) == 0) {
        payload_off = 0;
    } else if (pkt_len > (prefix_len + 2) &&
               strncmp(pkt + 2, prefix, prefix_len) == 0) {
        /* ipourma may carry a 2-byte private header before UDP payload. */
        payload_off = 2;
    } else {
        if (g_rx_debug_drops < 16) {
            char src_buf[INET_ADDRSTRLEN];
            unsigned char b0 = (unsigned char)pkt[0];
            unsigned char b1 = (unsigned char)((pkt_len > 1) ? pkt[1] : 0);
            unsigned char b2 = (unsigned char)((pkt_len > 2) ? pkt[2] : 0);
            unsigned char b3 = (unsigned char)((pkt_len > 3) ? pkt[3] : 0);
            inet_ntop(AF_INET, src_addr, src_buf, sizeof(src_buf));
            fprintf(stderr,
                    "[urma_dp] drop: bad prefix src=%s len=%zu b=%02x %02x %02x %02x\n",
                    src_buf, pkt_len, b0, b1, b2, b3);
            g_rx_debug_drops++;
        }
        return 0;
    }

    body = pkt + payload_off;
    body_len = pkt_len - (size_t)payload_off;
    if (body_len <= prefix_len + 1) {
        return 0;
    }
    role_begin = body + prefix_len;
    role_end = memchr(role_begin, ':', body_len - prefix_len);
    if (role_end == NULL) {
        return 0;
    }
    {
        size_t role_len = (size_t)(role_end - role_begin);
        if (role_len == 0) {
            return 0;
        }
        local_role_msg = (strlen(local_role) == role_len &&
                          strncmp(local_role, role_begin, role_len) == 0);
    }

    if (local_role_msg) {
        if (g_rx_debug_drops < 16) {
            char src_buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, src_addr, src_buf, sizeof(src_buf));
            fprintf(stderr, "[urma_dp] drop: local role src=%s payload=%s\n",
                    src_buf, body);
            g_rx_debug_drops++;
        }
        return 0;
    }

    {
        size_t copy_len = body_len;
        if (copy_len >= payload_len) {
            copy_len = payload_len - 1;
        }
        memcpy(payload, body, copy_len);
        payload[copy_len] = '\0';
    }
    if (g_log_peer_rx) {
        char src_buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, src_addr, src_buf, sizeof(src_buf));
        printf("[urma_dp] rx peer src=%s ifindex=%u payload=%s\n",
               src_buf, rx_ifindex, payload);
    }
    return 1;
}

static int try_recv_peer(int sockfd, int rawfd, unsigned int ifindex,
                         const char *local_role,
                         char *payload, size_t payload_len)
{
    unsigned int drain;

    for (drain = 0; drain < 64; drain++) {
        struct sockaddr_in src;
        struct iovec iov;
        struct msghdr msg;
        char cbuf[256];
        ssize_t n;
        struct cmsghdr *cmsg;
        unsigned int rx_ifindex = 0;
        int rc;

        memset(&src, 0, sizeof(src));
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = payload;
        iov.iov_len = payload_len - 1;

        msg.msg_name = &src;
        msg.msg_namelen = sizeof(src);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);

        n = recvmsg(sockfd, &msg, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            fprintf(stderr, "[urma_dp] recvmsg failed: %s\n", strerror(errno));
            return -1;
        }
        payload[n] = '\0';

        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_PKTINFO) {
                struct in_pktinfo *pi = (struct in_pktinfo *)CMSG_DATA(cmsg);
                rx_ifindex = pi->ipi_ifindex;
                break;
            }
        }

        rc = consume_probe_payload(payload, (size_t)n, &src.sin_addr,
                                   rx_ifindex, ifindex, local_role,
                                   payload, payload_len);
        if (rc > 0) {
            return 1;
        }
    }

    if (rawfd >= 0) {
        uint8_t raw_buf[2048];

        for (drain = 0; drain < 64; drain++) {
            ssize_t n;
            struct iphdr *iph;
            struct udphdr *uh;
            struct in_addr src_addr;
            size_t ihl;
            size_t udp_off;
            size_t udp_len;
            int rc;

            n = recv(rawfd, raw_buf, sizeof(raw_buf), MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                fprintf(stderr, "[urma_dp] raw recv failed: %s\n", strerror(errno));
                return -1;
            }
            if ((size_t)n < sizeof(struct iphdr)) {
                continue;
            }

            iph = (struct iphdr *)raw_buf;
            if (iph->version != 4 || iph->ihl < 5 || iph->protocol != IPPROTO_UDP) {
                continue;
            }

            ihl = (size_t)iph->ihl * 4;
            udp_off = ihl;
            if ((size_t)n < udp_off + sizeof(struct udphdr)) {
                continue;
            }

            uh = (struct udphdr *)(raw_buf + udp_off);
            if (ntohs(uh->dest) != DP_PORT) {
                continue;
            }

            udp_len = (size_t)n - udp_off - sizeof(struct udphdr);
            src_addr.s_addr = iph->saddr;
            rc = consume_probe_payload((const char *)(raw_buf + udp_off + sizeof(struct udphdr)),
                                       udp_len, &src_addr, ifindex, ifindex, local_role,
                                       payload, payload_len);
            if (rc > 0) {
                return 1;
            }
        }
    }

    return 0;
}

static void bench_consume_rx(const char *payload,
                             unsigned int *bench_rx_pkts,
                             bool *peer_done,
                             unsigned int *peer_tx_pkts)
{
    char peer_role[32];
    char peer_nonce[32];
    char kind[32];
    unsigned int value;

    if (sscanf(payload, "urma-dp:%31[^:]:%31[^:]:%31[^:]:%u",
               peer_role, peer_nonce, kind, &value) != 4) {
        return;
    }

    if (strcmp(kind, "bench") == 0) {
        *bench_rx_pkts += 1;
        return;
    }

    if (strcmp(kind, "done") == 0) {
        *peer_done = true;
        *peer_tx_pkts = value;
    }
}

static int run_bench_phase(int sockfd, int rawfd, unsigned int ifindex,
                           const char *role,
                           const struct sockaddr_in *unicast_addr,
                           uint32_t nonce,
                           unsigned int bench_pkts,
                           unsigned int bench_interval_us,
                           unsigned int bench_wait_ms,
                           unsigned int bench_min_rx_pps,
                           unsigned int bench_max_loss_ppm)
{
    unsigned int tx_ok = 0;
    unsigned int rx_ok = 0;
    unsigned int peer_tx = 0;
    unsigned int loss_pkts;
    unsigned int loss_ppm;
    unsigned int tx_pps;
    unsigned int rx_pps;
    bool peer_done = false;
    long tx_start_ms;
    long tx_end_ms;
    long rx_end_ms;
    long tx_window_ms;
    long rx_window_ms;
    unsigned int i;
    int ret = 0;
    char tx[160];
    char rx[256];

    if (bench_pkts == 0) {
        return 0;
    }

    fprintf(stderr,
            "[urma_dp] bench start pkts=%u interval_us=%u wait_ms=%u min_rx_pps=%u max_loss_ppm=%u\n",
            bench_pkts, bench_interval_us, bench_wait_ms,
            bench_min_rx_pps, bench_max_loss_ppm);

    g_log_peer_rx = false;
    tx_start_ms = now_ms();

    for (i = 0; i < bench_pkts && !g_alarm_fired; i++) {
        ssize_t sn;
        int got;

        snprintf(tx, sizeof(tx), "urma-dp:%s:%08" PRIx32 ":bench:%u",
                 role, nonce, i);
        sn = sendto(sockfd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)unicast_addr, sizeof(*unicast_addr));
        if (sn >= 0) {
            tx_ok++;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[urma_dp] warn: bench send failed: %s\n", strerror(errno));
        }

        got = try_recv_peer(sockfd, rawfd, ifindex, role, rx, sizeof(rx));
        if (got < 0) {
            ret = 1;
            goto out;
        }
        if (got > 0) {
            bench_consume_rx(rx, &rx_ok, &peer_done, &peer_tx);
        }

        if (bench_interval_us != 0) {
            usleep(bench_interval_us);
        }
    }

    tx_end_ms = now_ms();
    snprintf(tx, sizeof(tx), "urma-dp:%s:%08" PRIx32 ":done:%u", role, nonce, tx_ok);
    for (i = 0; i < 16 && !g_alarm_fired; i++) {
        ssize_t sn;

        sn = sendto(sockfd, tx, strlen(tx), MSG_DONTWAIT,
                    (const struct sockaddr *)unicast_addr, sizeof(*unicast_addr));
        if (sn >= 0) {
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[urma_dp] warn: bench done send failed: %s\n", strerror(errno));
            break;
        }
        usleep(5000);
    }

    {
        long rx_deadline_ms = now_ms() + (long)bench_wait_ms;
        long next_done_resend_ms = now_ms() + 200;
        while (!g_alarm_fired && now_ms() < rx_deadline_ms) {
            int got = try_recv_peer(sockfd, rawfd, ifindex, role, rx, sizeof(rx));
            if (got < 0) {
                ret = 1;
                goto out;
            }
            if (got > 0) {
                bench_consume_rx(rx, &rx_ok, &peer_done, &peer_tx);
                if (peer_done) {
                    break;
                }
            } else {
                if (now_ms() >= next_done_resend_ms) {
                    (void)sendto(sockfd, tx, strlen(tx), MSG_DONTWAIT,
                                 (const struct sockaddr *)unicast_addr,
                                 sizeof(*unicast_addr));
                    next_done_resend_ms = now_ms() + 200;
                }
                usleep(1000);
            }
        }
    }

    if (!peer_done) {
        fprintf(stderr, "[urma_dp] warn: bench peer done not received, fallback peer_tx=%u\n",
                bench_pkts);
        peer_tx = bench_pkts;
    }

    rx_end_ms = now_ms();
    tx_window_ms = tx_end_ms - tx_start_ms;
    rx_window_ms = rx_end_ms - tx_start_ms;
    if (tx_window_ms <= 0) {
        tx_window_ms = 1;
    }
    if (rx_window_ms <= 0) {
        rx_window_ms = 1;
    }

    tx_pps = (unsigned int)(((uint64_t)tx_ok * 1000ULL) / (uint64_t)tx_window_ms);
    rx_pps = (unsigned int)(((uint64_t)rx_ok * 1000ULL) / (uint64_t)rx_window_ms);
    loss_pkts = (peer_tx > rx_ok) ? (peer_tx - rx_ok) : 0;
    loss_ppm = (peer_tx == 0) ? 0 :
               (unsigned int)(((uint64_t)loss_pkts * 1000000ULL) / (uint64_t)peer_tx);

    printf("[urma_dp] bench summary tx=%u rx=%u peer_tx=%u loss=%u loss_ppm=%u tx_pps=%u rx_pps=%u\n",
           tx_ok, rx_ok, peer_tx, loss_pkts, loss_ppm, tx_pps, rx_pps);

    if (rx_pps < bench_min_rx_pps) {
        fprintf(stderr, "[urma_dp] fail: bench rx_pps=%u below min_rx_pps=%u\n",
                rx_pps, bench_min_rx_pps);
        ret = 1;
        goto out;
    }

    if (loss_ppm > bench_max_loss_ppm) {
        fprintf(stderr, "[urma_dp] fail: bench loss_ppm=%u exceeds max_loss_ppm=%u\n",
                loss_ppm, bench_max_loss_ppm);
        ret = 1;
        goto out;
    }

    printf("[urma_dp] bench pass\n");

out:
    g_log_peer_rx = true;
    return ret;
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
    struct sockaddr_in bcast_addr;
    struct sockaddr_in unicast_addr;
    struct timeval tv;
    int one = 1;
    int sockfd = -1;
    int rawfd = -1;
    unsigned int ifindex = 0;
    bool has_peer = false;
    uint32_t nonce;
    long deadline;
    int max_seq;
    int seq = 0;
    unsigned int bench_pkts;
    unsigned int bench_interval_us;
    unsigned int bench_wait_ms;
    unsigned int bench_min_rx_pps;
    unsigned int bench_max_loss_ppm;
    struct sigaction sa;

    (void)cmdline_get_value("linqu_urma_dp_role", role, sizeof(role));
    bench_pkts = cmdline_get_u32_default("linqu_urma_dp_bench_pkts", 0);
    bench_interval_us = cmdline_get_u32_default("linqu_urma_dp_bench_interval_us", 1000);
    bench_wait_ms = cmdline_get_u32_default("linqu_urma_dp_bench_wait_ms", 5000);
    bench_min_rx_pps = cmdline_get_u32_default("linqu_urma_dp_bench_min_rx_pps", 0);
    bench_max_loss_ppm = cmdline_get_u32_default("linqu_urma_dp_bench_max_loss_ppm", 1000000);

    printf("[urma_dp] start role=%s\n", role);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = alarm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGALRM, &sa, NULL);
    alarm((RUN_TIMEOUT_MS / 1000) + 5);

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[urma_dp] fail: ipourma iface not ready\n");
        return 1;
    }

    /* assign static IPv4: nodeA=10.0.0.1, nodeB=10.0.0.2 */
    const char *my_ip;
    const char *peer_ip;
    if (strcmp(role, "nodeA") == 0) {
        my_ip = "10.0.0.1";
        peer_ip = "10.0.0.2";
    } else if (strcmp(role, "nodeB") == 0) {
        my_ip = "10.0.0.2";
        peer_ip = "10.0.0.1";
    } else {
        fprintf(stderr, "[urma_dp] fail: unknown role '%s'\n", role);
        return 1;
    }

    if (!set_ipv4_addr(ifname, my_ip)) {
        fprintf(stderr, "[urma_dp] fail: set ipv4 %s on %s failed\n", my_ip, ifname);
        return 1;
    }

    if (!get_local_ipv4(ifname, &local_addr)) {
        fprintf(stderr, "[urma_dp] fail: get ipv4 addr on %s failed\n", ifname);
        return 1;
    }

    inet_pton(AF_INET, peer_ip, &peer_addr);
    has_peer = true;
    install_static_arp(ifname, &peer_addr);

    {
        char buf[INET_ADDRSTRLEN];
        printf("[urma_dp] iface=%s ifindex=%u local=%s peer=%s\n",
               ifname, ifindex,
               inet_ntop(AF_INET, &local_addr, buf, sizeof(buf)),
               inet_ntop(AF_INET, &peer_addr, (char[INET_ADDRSTRLEN]){0}, INET_ADDRSTRLEN));
    }

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "[urma_dp] fail: socket create failed: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[urma_dp] socket created\n");

    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[urma_dp] fail: SO_REUSEADDR failed: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }

    /* Force software UDP checksum offload avoidance on this test socket. */
    if (setsockopt(sockfd, SOL_SOCKET, SO_NO_CHECK, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[urma_dp] warn: SO_NO_CHECK failed: %s\n", strerror(errno));
    }

    {
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags >= 0 && fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == 0) {
            printf("[urma_dp] socket set nonblocking\n");
        } else {
            fprintf(stderr, "[urma_dp] warn: set nonblocking failed: %s\n", strerror(errno));
        }
    }

    if (setsockopt(sockfd, IPPROTO_IP, IP_PKTINFO, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[urma_dp] warn: IP_PKTINFO failed: %s\n", strerror(errno));
    }

    if (setsockopt(sockfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) != 0) {
        fprintf(stderr, "[urma_dp] warn: SO_BINDTODEVICE failed: %s\n", strerror(errno));
    }

    rawfd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
    if (rawfd < 0) {
        fprintf(stderr, "[urma_dp] warn: raw socket create failed: %s\n", strerror(errno));
    } else {
        if (setsockopt(rawfd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) != 0) {
            fprintf(stderr, "[urma_dp] warn: raw SO_BINDTODEVICE failed: %s\n", strerror(errno));
        }
        {
            int flags = fcntl(rawfd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(rawfd, F_SETFL, flags | O_NONBLOCK);
            }
        }
    }

    tv.tv_sec = RX_TIMEOUT_MS / 1000;
    tv.tv_usec = (RX_TIMEOUT_MS % 1000) * 1000;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        fprintf(stderr, "[urma_dp] fail: SO_RCVTIMEO failed: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }

    {
        struct sockaddr_in bind_addr;

        memset(&bind_addr, 0, sizeof(bind_addr));
        bind_addr.sin_family = AF_INET;
        bind_addr.sin_port = htons(DP_PORT);
        bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(sockfd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) != 0) {
            fprintf(stderr, "[urma_dp] fail: bind port %d failed: %s\n",
                    DP_PORT, strerror(errno));
            close(sockfd);
            return 1;
        }
    }
    fprintf(stderr, "[urma_dp] bind ok port=%d\n", DP_PORT);

    /* broadcast address for discovery */
    memset(&bcast_addr, 0, sizeof(bcast_addr));
    bcast_addr.sin_family = AF_INET;
    bcast_addr.sin_port = htons(DP_PORT);
    inet_pton(AF_INET, "10.0.0.255", &bcast_addr.sin_addr);

    /* unicast to peer */
    memset(&unicast_addr, 0, sizeof(unicast_addr));
    unicast_addr.sin_family = AF_INET;
    unicast_addr.sin_port = htons(DP_PORT);
    unicast_addr.sin_addr = peer_addr;

    /* enable broadcast */
    if (setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) != 0) {
        fprintf(stderr, "[urma_dp] warn: SO_BROADCAST failed: %s\n", strerror(errno));
    }

    nonce = (uint32_t)getpid() ^ (uint32_t)time(NULL);
    deadline = now_ms() + RUN_TIMEOUT_MS;
    max_seq = 40;
    fprintf(stderr, "[urma_dp] enter probe loop\n");

    while (!g_alarm_fired) {
        char tx[160];
        char rx[256];
        int got;
        ssize_t sn;
        if (now_ms() >= deadline) {
            break;
        }
        if (seq >= max_seq) {
            break;
        }
        if ((seq % 200) == 0) {
            fprintf(stderr, "[urma_dp] loop seq=%d now=%ld deadline=%ld\n",
                    seq, now_ms(), deadline);
        }

        snprintf(tx, sizeof(tx), "urma-dp:%s:%08" PRIx32 ":%d", role, nonce, seq++);

        if (!has_peer) {
            fprintf(stderr, "[urma_dp] send bcast begin\n");
            sn = sendto(sockfd, tx, strlen(tx), MSG_DONTWAIT,
                        (const struct sockaddr *)&bcast_addr, sizeof(bcast_addr));
            fprintf(stderr, "[urma_dp] send bcast end ret=%zd err=%d\n", sn, errno);
            if (sn < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    fprintf(stderr, "[urma_dp] warn: bcast send failed: %s\n", strerror(errno));
                }
            }
        }

        if (has_peer) {
            fprintf(stderr, "[urma_dp] send unicast begin\n");
            sn = sendto(sockfd, tx, strlen(tx), MSG_DONTWAIT,
                        (const struct sockaddr *)&unicast_addr, sizeof(unicast_addr));
            fprintf(stderr, "[urma_dp] send unicast end ret=%zd err=%d\n", sn, errno);
            if (sn < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "[urma_dp] warn: unicast send failed: %s\n", strerror(errno));
            }
        }

        got = try_recv_peer(sockfd, rawfd, ifindex, role, rx, sizeof(rx));
        if (got < 0) {
            if (rawfd >= 0) {
                close(rawfd);
            }
            close(sockfd);
            return 1;
        }
        if (got > 0) {
            printf("[urma_dp] pass\n");
            if (bench_pkts > 0 &&
                run_bench_phase(sockfd, rawfd, ifindex, role, &unicast_addr,
                                nonce, bench_pkts, bench_interval_us, bench_wait_ms,
                                bench_min_rx_pps, bench_max_loss_ppm) != 0) {
                if (rawfd >= 0) {
                    close(rawfd);
                }
                close(sockfd);
                return 1;
            }
            if (rawfd >= 0) {
                close(rawfd);
            }
            close(sockfd);
            return 0;
        }

        usleep(150000);
    }

    if (g_alarm_fired) {
        fprintf(stderr, "[urma_dp] fail: alarm timeout fired\n");
        if (rawfd >= 0) {
            close(rawfd);
        }
        close(sockfd);
        return 1;
    }

    fprintf(stderr, "[urma_dp] fail: timeout waiting peer packet\n");
    if (rawfd >= 0) {
        close(rawfd);
    }
    close(sockfd);
    return 1;
}
