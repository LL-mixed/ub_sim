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
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TCP_EACH_SERVER_PORT 18620
#define RUN_TIMEOUT_S        30
#define IO_TIMEOUT_S         5
#define WAIT_IFACE_MS        90000
#define RETRY_DELAY_US       200000
#define TCP_BENCH_DEFAULT_SIZE       (2UL * 1024UL * 1024UL)
#define TCP_BENCH_DEFAULT_ITERATIONS 32768ULL
#define TCP_BENCH_DEFAULT_CHUNK_SIZE 64ULL

struct tcp_bench_config {
    bool enabled;
    bool verify;
    uint64_t size;
    uint64_t iterations;
    uint64_t chunk_size;
};

struct tcp_bench_stats {
    uint64_t reads;
    uint64_t writes;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t verify_failures;
    long duration_ms;
};

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

static bool env_flag_enabled(const char *key)
{
    const char *value = getenv(key);

    if (!value || value[0] == '\0') {
        return false;
    }
    return strcmp(value, "0") != 0 && strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0 && strcmp(value, "no") != 0 &&
           strcmp(value, "NO") != 0;
}

static uint64_t env_u64_or_default(const char *key, uint64_t fallback)
{
    const char *value = getenv(key);
    char *end = NULL;
    uint64_t parsed;

    if (!value || value[0] == '\0') {
        return fallback;
    }
    errno = 0;
    parsed = strtoull(value, &end, 0);
    if (errno != 0 || end == NULL || *end != '\0') {
        return fallback;
    }
    return parsed;
}

static void tcp_bench_init_config(struct tcp_bench_config *cfg)
{
    cfg->enabled = env_flag_enabled("TCP_BENCHMARK") ||
                   env_flag_enabled("LINQU_TCP_BENCHMARK");
    cfg->verify = env_flag_enabled("TCP_BENCH_VERIFY") ||
                  env_flag_enabled("LINQU_TCP_BENCH_VERIFY");
    cfg->size = env_u64_or_default("TCP_BENCH_SIZE", TCP_BENCH_DEFAULT_SIZE);
    cfg->iterations = env_u64_or_default("TCP_BENCH_ITERATIONS",
                                         TCP_BENCH_DEFAULT_ITERATIONS);
    cfg->chunk_size = env_u64_or_default("TCP_BENCH_CHUNK_SIZE",
                                         TCP_BENCH_DEFAULT_CHUNK_SIZE);
    if (cfg->size == 0) {
        cfg->size = TCP_BENCH_DEFAULT_SIZE;
    }
    if (cfg->iterations == 0) {
        cfg->iterations = TCP_BENCH_DEFAULT_ITERATIONS;
    }
    if (cfg->chunk_size == 0 || cfg->chunk_size > cfg->size) {
        cfg->chunk_size = TCP_BENCH_DEFAULT_CHUNK_SIZE;
    }
}

static void tcp_bench_fill_pattern(uint8_t *buf, uint64_t len, uint64_t seed)
{
    uint64_t i;

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)((seed + i) * 0x9e3779b9 + 0x85ebca77);
    }
}

static bool tcp_bench_verify_pattern(const uint8_t *buf, uint64_t len, uint64_t seed)
{
    uint64_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] != (uint8_t)((seed + i) * 0x9e3779b9 + 0x85ebca77)) {
            return false;
        }
    }
    return true;
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
    bool have_local = env_or_cmdline_value("LINQU_UB_LOCAL_IP",
                                           "linqu_ipourma_ipv4",
                                           local, local_len);
    bool have_peer = env_or_cmdline_value("LINQU_UB_PEER_IP",
                                          "linqu_ipourma_peer_ipv4",
                                          peer, peer_len);

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
        usleep(RETRY_DELAY_US);
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
        fprintf(stderr, "[ub_tcp_each_server] fail: set_ipv4 socket: %s\n",
                strerror(errno));
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        fprintf(stderr, "[ub_tcp_each_server] fail: invalid IPv4 %s\n", addr_str);
        close(fd);
        return false;
    }

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: SIOCSIFADDR %s: %s\n",
                ifname, strerror(errno));
        close(fd);
        return false;
    }

    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: SIOCSIFNETMASK %s: %s\n",
                ifname, strerror(errno));
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
        fprintf(stderr, "[ub_tcp_each_server] warn: socket for SIOCSARP failed: %s\n",
                strerror(errno));
        return;
    }

    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] warn: SIOCSARP %s failed: %s\n",
                ifname, strerror(errno));
    }

    close(fd);
}

static void set_socket_timeouts(int fd)
{
    struct timeval tv;

    tv.tv_sec = IO_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void bind_socket_to_iface(int fd, const char *ifname)
{
#ifdef SO_BINDTODEVICE
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) != 0) {
        if (errno == ENOPROTOOPT || errno == EOPNOTSUPP || errno == ENODEV) {
            return;
        }
        fprintf(stderr, "[ub_tcp_each_server] warn: SO_BINDTODEVICE %s: %s\n",
                ifname, strerror(errno));
    }
#else
    (void)fd;
    (void)ifname;
#endif
}

static int create_listener(const char *ifname, const char *local_ip)
{
    struct sockaddr_in addr;
    int one = 1;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: socket(listener): %s\n",
                strerror(errno));
        return -1;
    }

    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    bind_socket_to_iface(fd, ifname);
    set_socket_timeouts(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TCP_EACH_SERVER_PORT);
    if (inet_pton(AF_INET, local_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[ub_tcp_each_server] fail: inet_pton local %s\n", local_ip);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: bind listener %s:%d: %s\n",
                local_ip, TCP_EACH_SERVER_PORT, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 4) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: listen: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

static int wait_for_accept(int listener_fd, long deadline_ms)
{
    while (now_ms() < deadline_ms) {
        fd_set rfds;
        struct timeval tv;

        FD_ZERO(&rfds);
        FD_SET(listener_fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 250000;

        if (select(listener_fd + 1, &rfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[ub_tcp_each_server] fail: select(accept): %s\n",
                    strerror(errno));
            return -1;
        }

        if (FD_ISSET(listener_fd, &rfds)) {
            int accepted_fd = accept(listener_fd, NULL, NULL);
            if (accepted_fd >= 0) {
                set_socket_timeouts(accepted_fd);
                return accepted_fd;
            }
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "[ub_tcp_each_server] fail: accept: %s\n",
                        strerror(errno));
                return -1;
            }
        }
    }

    fprintf(stderr, "[ub_tcp_each_server] fail: accept timeout\n");
    return -1;
}

static bool restore_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0) {
        return false;
    }

    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0;
}

static int connect_to_peer_retry(const char *ifname, const char *local_ip,
                                 const char *peer_ip, long deadline_ms)
{
    while (now_ms() < deadline_ms) {
        struct sockaddr_in local_addr;
        struct sockaddr_in peer_addr;
        int one = 1;
        int fd;
        int flags;
        int ret;

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            fprintf(stderr, "[ub_tcp_each_server] fail: socket(connect): %s\n",
                    strerror(errno));
            return -1;
        }

        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        bind_socket_to_iface(fd, ifname);

        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_port = htons(0);
        inet_pton(AF_INET, local_ip, &local_addr.sin_addr);
        if (bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0) {
            fprintf(stderr, "[ub_tcp_each_server] warn: bind client %s: %s\n",
                    local_ip, strerror(errno));
            close(fd);
            return -1;
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            fprintf(stderr, "[ub_tcp_each_server] fail: fcntl nonblock: %s\n",
                    strerror(errno));
            close(fd);
            return -1;
        }

        memset(&peer_addr, 0, sizeof(peer_addr));
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_port = htons(TCP_EACH_SERVER_PORT);
        if (inet_pton(AF_INET, peer_ip, &peer_addr.sin_addr) != 1) {
            fprintf(stderr, "[ub_tcp_each_server] fail: inet_pton peer %s\n", peer_ip);
            close(fd);
            return -1;
        }

        ret = connect(fd, (struct sockaddr *)&peer_addr, sizeof(peer_addr));
        if (ret == 0) {
            restore_blocking(fd);
            set_socket_timeouts(fd);
            return fd;
        }

        if (errno == EINPROGRESS || errno == EALREADY || errno == EWOULDBLOCK) {
            fd_set wfds;
            struct timeval tv;
            int so_error = 0;
            socklen_t so_error_len = sizeof(so_error);

            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = 0;
            tv.tv_usec = 250000;

            ret = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (ret > 0 && FD_ISSET(fd, &wfds) &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_len) == 0 &&
                so_error == 0) {
                restore_blocking(fd);
                set_socket_timeouts(fd);
                return fd;
            }
        } else if (errno != ECONNREFUSED && errno != ENETUNREACH &&
                   errno != EHOSTUNREACH) {
            fprintf(stderr, "[ub_tcp_each_server] warn: connect %s:%d failed: %s\n",
                    peer_ip, TCP_EACH_SERVER_PORT, strerror(errno));
        }

        close(fd);
        usleep(RETRY_DELAY_US);
    }

    fprintf(stderr, "[ub_tcp_each_server] fail: connect timeout to %s:%d\n",
            peer_ip, TCP_EACH_SERVER_PORT);
    return -1;
}

static bool send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *bytes = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, bytes + off, len - off, 0);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        fprintf(stderr, "[ub_tcp_each_server] fail: send: %s\n", strerror(errno));
        return false;
    }

    return true;
}

static bool recv_all(int fd, void *buf, size_t len)
{
    uint8_t *bytes = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t n = recv(fd, bytes + off, len - off, 0);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n == 0) {
            fprintf(stderr, "[ub_tcp_each_server] fail: unexpected EOF\n");
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        fprintf(stderr, "[ub_tcp_each_server] fail: recv: %s\n", strerror(errno));
        return false;
    }

    return true;
}

static bool send_line(int fd, const char *line)
{
    char buf[256];

    if (snprintf(buf, sizeof(buf), "%s\n", line) >= (int)sizeof(buf)) {
        fprintf(stderr, "[ub_tcp_each_server] fail: payload too long\n");
        return false;
    }

    return send_all(fd, buf, strlen(buf));
}

static bool recv_line(int fd, char *buf, size_t buf_len)
{
    size_t off = 0;

    while (off + 1 < buf_len) {
        char ch;
        ssize_t n = recv(fd, &ch, 1, 0);

        if (n > 0) {
            if (ch == '\n') {
                buf[off] = '\0';
                return true;
            }
            buf[off++] = ch;
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        fprintf(stderr, "[ub_tcp_each_server] fail: recv: %s\n", strerror(errno));
        return false;
    }

    buf[off] = '\0';
    if (off > 0) {
        return true;
    }

    fprintf(stderr, "[ub_tcp_each_server] fail: unexpected EOF\n");
    return false;
}

static bool role_messages(const char *role,
                          const char **client_msg,
                          const char **expected_server_msg,
                          const char **server_ack,
                          const char **expected_client_ack)
{
    if (strcmp(role, "nodeA") == 0) {
        *client_msg = "tcp hello from nodeA client";
        *expected_server_msg = "tcp hello from nodeB client";
        *server_ack = "tcp ack from nodeA server";
        *expected_client_ack = "tcp ack from nodeB server";
        return true;
    }
    if (strcmp(role, "nodeB") == 0) {
        *client_msg = "tcp hello from nodeB client";
        *expected_server_msg = "tcp hello from nodeA client";
        *server_ack = "tcp ack from nodeB server";
        *expected_client_ack = "tcp ack from nodeA server";
        return true;
    }
    return false;
}

static int run_server_child(const char *role, int listener_fd,
                            const char *expected_server_msg,
                            const char *server_ack)
{
    char recv_buf[256];
    int accepted_fd;

    accepted_fd = wait_for_accept(listener_fd, now_ms() + RUN_TIMEOUT_S * 1000L);
    if (accepted_fd < 0) {
        return 1;
    }

    if (!recv_line(accepted_fd, recv_buf, sizeof(recv_buf))) {
        close(accepted_fd);
        return 1;
    }

    if (strcmp(recv_buf, expected_server_msg) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: role=%s unexpected server payload=\"%s\" expected=\"%s\"\n",
                role, recv_buf, expected_server_msg);
        close(accepted_fd);
        return 1;
    }

    printf("[TCP_EACH_SERVER] %s server received=\"%s\"\n", role, recv_buf);
    if (!send_line(accepted_fd, server_ack)) {
        close(accepted_fd);
        return 1;
    }
    printf("[TCP_EACH_SERVER] %s server ack=\"%s\"\n", role, server_ack);

    close(accepted_fd);
    return 0;
}

static int run_benchmark_server_child(const char *role, int listener_fd,
                                      const struct tcp_bench_config *cfg)
{
    uint8_t *buf;
    uint64_t iter;
    int accepted_fd;
    uint64_t verify_failures = 0;

    accepted_fd = wait_for_accept(listener_fd, now_ms() + RUN_TIMEOUT_S * 1000L);
    if (accepted_fd < 0) {
        return 1;
    }

    buf = malloc((size_t)cfg->chunk_size);
    if (!buf) {
        fprintf(stderr, "[ub_tcp_each_server] fail: benchmark server alloc\n");
        close(accepted_fd);
        return 1;
    }

    for (iter = 0; iter < cfg->iterations; iter++) {
        uint64_t offset = (iter * cfg->chunk_size) % cfg->size;
        uint64_t seed = offset ^ iter;

        if (!recv_all(accepted_fd, buf, (size_t)cfg->chunk_size)) {
            free(buf);
            close(accepted_fd);
            return 1;
        }
        if (cfg->verify &&
            !tcp_bench_verify_pattern(buf, cfg->chunk_size, seed)) {
            verify_failures++;
        }
        if (!send_all(accepted_fd, buf, (size_t)cfg->chunk_size)) {
            free(buf);
            close(accepted_fd);
            return 1;
        }
    }

    printf("[ub_tcp_each_server] benchmark_server role=%s iterations=%" PRIu64
           " chunk_size=%" PRIu64 " verify_failures=%" PRIu64 "\n",
           role, cfg->iterations, cfg->chunk_size, verify_failures);
    free(buf);
    close(accepted_fd);
    return verify_failures == 0 ? 0 : 1;
}

static bool run_benchmark_client(int fd, const char *role,
                                 const struct tcp_bench_config *cfg,
                                 struct tcp_bench_stats *stats)
{
    uint8_t *tmp_write;
    uint8_t *tmp_read;
    uint64_t iter;
    long t0;

    memset(stats, 0, sizeof(*stats));
    tmp_write = malloc((size_t)cfg->chunk_size);
    tmp_read = malloc((size_t)cfg->chunk_size);
    if (!tmp_write || !tmp_read) {
        fprintf(stderr, "[ub_tcp_each_server] fail: benchmark client alloc\n");
        free(tmp_write);
        free(tmp_read);
        return false;
    }

    t0 = now_ms();
    for (iter = 0; iter < cfg->iterations; iter++) {
        uint64_t offset = (iter * cfg->chunk_size) % cfg->size;
        uint64_t seed = offset ^ iter;

        tcp_bench_fill_pattern(tmp_write, cfg->chunk_size, seed);
        if (!send_all(fd, tmp_write, (size_t)cfg->chunk_size)) {
            free(tmp_write);
            free(tmp_read);
            return false;
        }
        stats->writes++;
        stats->write_bytes += cfg->chunk_size;

        if (!recv_all(fd, tmp_read, (size_t)cfg->chunk_size)) {
            free(tmp_write);
            free(tmp_read);
            return false;
        }
        stats->reads++;
        stats->read_bytes += cfg->chunk_size;
        if (cfg->verify &&
            !tcp_bench_verify_pattern(tmp_read, cfg->chunk_size, seed)) {
            stats->verify_failures++;
        }
    }
    stats->duration_ms = now_ms() - t0;

    printf("[TCP_EACH_SERVER] %s benchmark client complete iterations=%" PRIu64 "\n",
           role, cfg->iterations);
    free(tmp_write);
    free(tmp_read);
    return true;
}

static void print_benchmark_result(const char *role,
                                   const struct tcp_bench_config *cfg,
                                   const struct tcp_bench_stats *stats)
{
    double dur_s = stats->duration_ms > 0 ? stats->duration_ms / 1000.0 : 0.001;
    double rmb = stats->read_bytes / (1024.0 * 1024.0);
    double wmb = stats->write_bytes / (1024.0 * 1024.0);

    printf("[ub_tcp_each_server] benchmark_result=done role=%s size=%" PRIu64
           " iterations=%" PRIu64 " chunk_size=%" PRIu64
           " reads=%" PRIu64 " writes=%" PRIu64
           " read_bytes=%" PRIu64 " write_bytes=%" PRIu64
           " verify_failures=%" PRIu64 " duration_ms=%ld"
           " read_mbps=%.3f write_mbps=%.3f\n",
           role, cfg->size, cfg->iterations, cfg->chunk_size,
           stats->reads, stats->writes, stats->read_bytes, stats->write_bytes,
           stats->verify_failures, stats->duration_ms, rmb / dur_s, wmb / dur_s);
}

static void cleanup_child(pid_t child_pid)
{
    if (child_pid <= 0) {
        return;
    }

    kill(child_pid, SIGKILL);
    waitpid(child_pid, NULL, 0);
}

int main(void)
{
    char role[32];
    char ifname[IFNAMSIZ];
    char local_ip[INET_ADDRSTRLEN];
    char peer_ip[INET_ADDRSTRLEN];
    struct in_addr local_addr;
    struct in_addr current_addr;
    struct in_addr peer_addr;
    unsigned int ifindex = 0;
    const char *client_msg = NULL;
    const char *expected_server_msg = NULL;
    const char *server_ack = NULL;
    const char *expected_client_ack = NULL;
    int listener_fd = -1;
    int client_fd = -1;
    int status = 0;
    pid_t child_pid = -1;
    char recv_buf[256];
    struct tcp_bench_config bench_cfg;
    struct tcp_bench_stats bench_stats;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
    tcp_bench_init_config(&bench_cfg);

    if (!env_or_cmdline_value("LINQU_URMA_DP_ROLE", "linqu_urma_dp_role",
                              role, sizeof(role))) {
        fprintf(stderr, "[ub_tcp_each_server] fail: missing linqu_urma_dp_role\n");
        return 1;
    }

    if (!resolve_ipv4_pair(role, local_ip, sizeof(local_ip), peer_ip, sizeof(peer_ip))) {
        fprintf(stderr, "[ub_tcp_each_server] fail: cannot resolve IPv4 pair for role=%s\n",
                role);
        return 1;
    }

    if (!role_messages(role, &client_msg, &expected_server_msg,
                       &server_ack, &expected_client_ack)) {
        fprintf(stderr, "[ub_tcp_each_server] fail: unsupported role=%s\n", role);
        return 1;
    }

    if (!wait_iface_ready(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[ub_tcp_each_server] fail: ipourma interface not ready\n");
        return 1;
    }

    if (inet_pton(AF_INET, local_ip, &local_addr) != 1 ||
        inet_pton(AF_INET, peer_ip, &peer_addr) != 1) {
        fprintf(stderr, "[ub_tcp_each_server] fail: inet_pton local/peer\n");
        return 1;
    }

    if (!get_local_ipv4(ifname, &current_addr) || current_addr.s_addr != local_addr.s_addr) {
        if (!set_ipv4_addr(ifname, local_ip)) {
            return 1;
        }
    }

    install_static_arp(ifname, &peer_addr);

    printf("[ub_tcp_each_server] start role=%s iface=%s ifindex=%u local=%s peer=%s port=%d"
           " benchmark=%d\n",
           role, ifname, ifindex, local_ip, peer_ip, TCP_EACH_SERVER_PORT,
           bench_cfg.enabled ? 1 : 0);

    listener_fd = create_listener(ifname, local_ip);
    if (listener_fd < 0) {
        return 1;
    }

    fflush(NULL);
    child_pid = fork();
    if (child_pid < 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: fork: %s\n", strerror(errno));
        close(listener_fd);
        return 1;
    }

    if (child_pid == 0) {
        if (bench_cfg.enabled) {
            status = run_benchmark_server_child(role, listener_fd, &bench_cfg);
        } else {
            status = run_server_child(role, listener_fd, expected_server_msg, server_ack);
        }
        close(listener_fd);
        _exit(status);
    }

    close(listener_fd);
    listener_fd = -1;

    client_fd = connect_to_peer_retry(ifname, local_ip, peer_ip,
                                      now_ms() + RUN_TIMEOUT_S * 1000L);
    if (client_fd < 0) {
        cleanup_child(child_pid);
        return 1;
    }

    if (bench_cfg.enabled) {
        if (!run_benchmark_client(client_fd, role, &bench_cfg, &bench_stats)) {
            close(client_fd);
            cleanup_child(child_pid);
            return 1;
        }
    } else {
        if (!send_line(client_fd, client_msg)) {
            close(client_fd);
            cleanup_child(child_pid);
            return 1;
        }
        printf("[TCP_EACH_SERVER] %s client sent=\"%s\"\n", role, client_msg);

        if (!recv_line(client_fd, recv_buf, sizeof(recv_buf))) {
            close(client_fd);
            cleanup_child(child_pid);
            return 1;
        }

        if (strcmp(recv_buf, expected_client_ack) != 0) {
            fprintf(stderr, "[ub_tcp_each_server] fail: role=%s unexpected client ack=\"%s\" expected=\"%s\"\n",
                    role, recv_buf, expected_client_ack);
            close(client_fd);
            cleanup_child(child_pid);
            return 1;
        }
        printf("[TCP_EACH_SERVER] %s client received_ack=\"%s\"\n", role, recv_buf);
    }
    close(client_fd);

    if (waitpid(child_pid, &status, 0) < 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: waitpid: %s\n", strerror(errno));
        return 1;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "[ub_tcp_each_server] fail: server child status=0x%x\n", status);
        return 1;
    }

    if (bench_cfg.enabled) {
        print_benchmark_result(role, &bench_cfg, &bench_stats);
        if (bench_stats.verify_failures != 0) {
            fprintf(stderr, "[ub_tcp_each_server] fail: benchmark verify_failures=%" PRIu64 "\n",
                    bench_stats.verify_failures);
            return 1;
        }
    }

    printf("[ub_tcp_each_server] summary role=%s local=%s peer=%s port=%d benchmark=%d\n",
           role, local_ip, peer_ip, TCP_EACH_SERVER_PORT,
           bench_cfg.enabled ? 1 : 0);
    printf("[ub_tcp_each_server] pass\n");
    return 0;
}
