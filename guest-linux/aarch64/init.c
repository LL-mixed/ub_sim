#define _GNU_SOURCE
#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdbool.h>
#include <dirent.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define UBC_PORT_SLICE_EMULATED_SIZE 0x800ULL
#define UBC_PORT0_SLICE_OFFSET 0x3400ULL
#define UBC_PORT_LINK_STATUS_OFFSET 0x700ULL
#define UBC_PORT_LINK_STATUS_UP 0x1
#define UBC_PORT1_SLICE_OFFSET (UBC_PORT0_SLICE_OFFSET + UBC_PORT_SLICE_EMULATED_SIZE)
#define UBC_PORT_NEIGHBOR_PORT_IDX_OFFSET 0x28ULL
#define UBC_PORT_NEIGHBOR_GUID_OFFSET 0x2cULL
#define UBC_PORT_GUID_SIZE 16
#define UBC_RESOURCE_BASE_FALLBACK 0x18000000000ULL
#define UB_REMOTE_CONFIG_PATH "/sys/bus/ub/devices/00002/config"
#define UB_CFG_UPI_OFFSET 0x7cULL

static bool read_file_line(const char *path, char *buf, size_t buf_size)
{
    int fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    n = read(fd, buf, buf_size - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }

    buf[n] = '\0';
    return true;
}

static bool should_enter_demo_boot_flow(void);

static void dump_raw_ubc_port1_state(void)
{
    static const char *paths[] = {
        "/sys/bus/ub/devices/00001/port1/linkup",
        "/sys/bus/ub/devices/00001/port1/neighbor_port_idx",
        "/sys/bus/ub/devices/00001/port1/neighbor_guid",
    };
    char buf[256];
    size_t i;

    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (read_file_line(paths[i], buf, sizeof(buf))) {
            fprintf(stderr, "[init] sysfs ubc port1 %s: %s\n",
                    strrchr(paths[i], '/') + 1, buf);
        } else {
            fprintf(stderr, "[init] sysfs ubc port1 %s: not available\n",
                    strrchr(paths[i], '/') + 1);
        }
    }
}

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "[init] mkdir %s failed: %s\n", path, strerror(errno));
    }
}

static void try_mount(const char *source, const char *target,
                      const char *fstype, unsigned long flags)
{
    if (mount(source, target, fstype, flags, NULL) != 0) {
        fprintf(stderr, "[init] mount %s on %s failed: %s\n",
                fstype, target, strerror(errno));
    }
}

static bool cmdline_has_option(const char *needle)
{
    int fd;
    ssize_t n;
    char buf[2048];

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }

    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

static bool cmdline_get_value(const char *key, char *out, size_t out_len)
{
    int fd;
    ssize_t n;
    char buf[2048];
    char *saveptr = NULL;
    char *tok;
    size_t key_len;

    fd = open("/proc/cmdline", O_RDONLY);
    if (fd < 0) {
        return false;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }

    buf[n] = '\0';
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

static bool should_run_linqu_probe(void)
{
    if (cmdline_has_option("linqu_probe_skip=1")) {
        return false;
    }
    return true;
}

static bool should_hold_after_probe(void)
{
    return cmdline_has_option("linqu_probe_hold=1");
}

static bool should_run_bizmsg_verify(void)
{
    return cmdline_has_option("linqu_bizmsg_verify=1");
}

static bool should_run_urma_dp_verify(void)
{
    return cmdline_has_option("linqu_urma_dp_verify=1");
}

static bool should_run_ub_chat(void)
{
    return cmdline_has_option("linqu_ub_chat=1");
}

static bool should_run_ub_rdma_demo(void)
{
    return cmdline_has_option("linqu_ub_rdma_demo=1");
}

static bool should_run_ub_tcp_each_server_demo(void)
{
    return cmdline_has_option("linqu_ub_tcp_each_server_demo=1");
}

static bool should_run_obmm_demo(void)
{
    return cmdline_has_option("linqu_obmm_demo=1");
}

static bool should_enter_demo_boot_flow(void)
{
    const char *flag = getenv("UB_RUN_DEMO_FROM_INIT");
    return flag != NULL && strcmp(flag, "1") == 0;
}

static bool should_run_ub_rpc_demo(void)
{
    return cmdline_has_option("linqu_ub_rpc_demo=1");
}

static bool read_interrupt_count(const char *name, uint64_t *count_out)
{
    FILE *fp;
    char line[512];

    fp = fopen("/proc/interrupts", "r");
    if (!fp) {
        fprintf(stderr, "[init] open /proc/interrupts failed: %s\n", strerror(errno));
        return false;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon;
        char *p;
        unsigned long long value;

        if (!strstr(line, name)) {
            continue;
        }

        colon = strchr(line, ':');
        if (!colon) {
            continue;
        }

        p = colon + 1;
        while (*p == ' ' || *p == '\t') {
            p++;
        }

        errno = 0;
        value = strtoull(p, NULL, 10);
        if (errno == 0) {
            *count_out = (uint64_t)value;
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

static bool touch_file_for_msg(const char *path)
{
    int fd;
    char buf[256];
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }

    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n >= 0;
}

struct bizmsg_payload_case {
    const char *name;
    uint64_t offset;
    uint32_t pattern;
    uint32_t mask;
};

static uint32_t nonzero_masked_u15(uint32_t value, uint32_t fallback)
{
    value &= 0x7fffU;
    if (value == 0) {
        return fallback & 0x7fffU;
    }
    return value;
}

static bool read_cfg_dword_fd(int fd, uint64_t offset, uint32_t *val_out)
{
    ssize_t n;

    n = pread(fd, val_out, sizeof(*val_out), (off_t)offset);
    if (n != (ssize_t)sizeof(*val_out)) {
        return false;
    }
    return true;
}

static bool write_cfg_dword_fd(int fd, uint64_t offset, uint32_t val)
{
    ssize_t n;

    n = pwrite(fd, &val, sizeof(val), (off_t)offset);
    if (n != (ssize_t)sizeof(val)) {
        return false;
    }
    return true;
}

static int run_bizmsg_payload_consistency_probe(uint64_t seed)
{
    struct bizmsg_payload_case cases[] = {
        {
            .name = "upi_case0",
            .offset = UB_CFG_UPI_OFFSET,
            .pattern = nonzero_masked_u15((uint32_t)seed ^ 0x1357U, 0x1U),
            .mask = 0x7fffU,
        },
        {
            .name = "upi_case1",
            .offset = UB_CFG_UPI_OFFSET,
            .pattern = nonzero_masked_u15((uint32_t)(seed >> 16) ^ 0x2a5aU, 0x2U),
            .mask = 0x7fffU,
        },
        {
            .name = "upi_case2",
            .offset = UB_CFG_UPI_OFFSET,
            .pattern = nonzero_masked_u15((uint32_t)(seed >> 32) ^ 0x55a5U, 0x4U),
            .mask = 0x7fffU,
        },
    };
    int fd;
    int errors = 0;
    size_t i;

    fd = open(UB_REMOTE_CONFIG_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[init] bizmsg payload fail: open %s failed: %s\n",
                UB_REMOTE_CONFIG_PATH, strerror(errno));
        return -1;
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const struct bizmsg_payload_case *c = &cases[i];
        uint32_t before = 0;
        uint32_t after = 0;
        uint32_t expect = 0;

        if (!read_cfg_dword_fd(fd, c->offset, &before)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: read-before name=%s pos=0x%08" PRIx64 " err=%s\n",
                    c->name, c->offset, strerror(errno));
            errors++;
            continue;
        }

        if (!write_cfg_dword_fd(fd, c->offset, c->pattern)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: write name=%s pos=0x%08" PRIx64 " val=0x%08" PRIx32 " err=%s\n",
                    c->name, c->offset, c->pattern, strerror(errno));
            errors++;
            continue;
        }

        if (!read_cfg_dword_fd(fd, c->offset, &after)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: read-after name=%s pos=0x%08" PRIx64 " err=%s\n",
                    c->name, c->offset, strerror(errno));
            errors++;
            (void)write_cfg_dword_fd(fd, c->offset, before);
            continue;
        }

        expect = c->pattern & c->mask;
        if ((after & c->mask) != expect) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: mismatch name=%s pos=0x%08" PRIx64
                    " tx=0x%08" PRIx32 " rx=0x%08" PRIx32 " mask=0x%08" PRIx32 "\n",
                    c->name, c->offset, c->pattern, after, c->mask);
            errors++;
        } else {
            fprintf(stderr,
                    "[init] bizmsg payload check pass name=%s pos=0x%08" PRIx64
                    " tx=0x%08" PRIx32 " rx=0x%08" PRIx32 " mask=0x%08" PRIx32 "\n",
                    c->name, c->offset, c->pattern, after, c->mask);
        }

        if (!write_cfg_dword_fd(fd, c->offset, before)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: restore-write name=%s pos=0x%08" PRIx64
                    " val=0x%08" PRIx32 " err=%s\n",
                    c->name, c->offset, before, strerror(errno));
            errors++;
            continue;
        }

        if (!read_cfg_dword_fd(fd, c->offset, &after)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: restore-read name=%s pos=0x%08" PRIx64
                    " err=%s\n",
                    c->name, c->offset, strerror(errno));
            errors++;
            continue;
        }

        if ((after & c->mask) != (before & c->mask)) {
            fprintf(stderr,
                    "[init] bizmsg payload fail: restore-mismatch name=%s pos=0x%08" PRIx64
                    " before=0x%08" PRIx32 " after=0x%08" PRIx32 " mask=0x%08" PRIx32 "\n",
                    c->name, c->offset, before, after, c->mask);
            errors++;
        }
    }

    close(fd);

    if (errors == 0) {
        fprintf(stderr, "[init] bizmsg payload pass cases=%zu\n",
                sizeof(cases) / sizeof(cases[0]));
        return 0;
    }

    fprintf(stderr, "[init] bizmsg payload fail errors=%d\n", errors);
    return -1;
}

static int run_bizmsg_roundtrip_probe(void)
{
    static const char *paths[] = {
        "/sys/bus/ub/devices/00002/port0/linkup",
        "/sys/bus/ub/devices/00002/port0/cna",
        "/sys/bus/ub/devices/00002/port0/neighbor",
        "/sys/bus/ub/devices/00002/port0/neighbor_guid",
        "/sys/bus/ub/devices/00002/guid",
        "/sys/bus/ub/devices/00002/resource",
    };
    char link_buf[64];
    uint64_t before = 0;
    uint64_t after = 0;
    int attempt;
    int i;
    size_t j;
    int errors = 0;

    /* In cluster mode, remote device 00002 may not appear in local sysfs */
    if (access("/sys/bus/ub/devices/00002", F_OK) != 0) {
        fprintf(stderr, "[init] bizmsg roundtrip skip: device 00002 not present\n");
        return 0;
    }

    for (attempt = 0; attempt < 100; attempt++) {
        if (read_file_line("/sys/bus/ub/devices/00002/port0/linkup",
                           link_buf, sizeof(link_buf)) &&
            strstr(link_buf, "1") != NULL) {
            break;
        }
        usleep(100000);
    }
    if (attempt == 100) {
        fprintf(stderr, "[init] bizmsg roundtrip fail: remote linkup not ready\n");
        return -1;
    }

    if (!read_interrupt_count("hi_msgq0-0", &before)) {
        fprintf(stderr, "[init] bizmsg roundtrip fail: hi_msgq0-0 missing before probe\n");
        return -1;
    }

    for (i = 0; i < 32; i++) {
        for (j = 0; j < sizeof(paths) / sizeof(paths[0]); j++) {
            if (!touch_file_for_msg(paths[j])) {
                fprintf(stderr, "[init] bizmsg read failed: %s (%s)\n", paths[j], strerror(errno));
                errors++;
            }
        }
    }

    if (run_bizmsg_payload_consistency_probe(before ^ (uint64_t)getpid()) != 0) {
        errors++;
    }

    usleep(500000);

    if (!read_interrupt_count("hi_msgq0-0", &after)) {
        fprintf(stderr, "[init] bizmsg roundtrip fail: hi_msgq0-0 missing after probe\n");
        return -1;
    }

    fprintf(stderr,
            "[init] bizmsg irq hi_msgq0-0 before=%" PRIu64 " after=%" PRIu64 " delta=%" PRIu64 "\n",
            before, after, (after >= before) ? (after - before) : 0);

    if (errors == 0 && after > before) {
        fprintf(stderr, "[init] bizmsg roundtrip pass\n");
        return 0;
    }

    fprintf(stderr, "[init] bizmsg roundtrip fail errors=%d\n", errors);
    return -1;
}

static void run_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_probe", "/bin/linqu_probe", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_probe failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] linqu_probe exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] linqu_probe signal=%d\n", WTERMSIG(status));
    }
}

static bool is_ipourma_ready(void)
{
    DIR *dir = opendir("/sys/class/net");
    struct dirent *entry;
    bool found = false;

    if (!dir) return false;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "ipourma", strlen("ipourma")) == 0) {
            found = true;
            break;
        }
    }
    closedir(dir);
    return found;
}

static bool find_ipourma_iface(char *name, size_t name_len, unsigned int *ifindex_out)
{
    DIR *dir;
    struct dirent *entry;

    dir = opendir("/sys/class/net");
    if (!dir) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        size_t n;

        if (strncmp(entry->d_name, "ipourma", strlen("ipourma")) != 0) {
            continue;
        }

        n = strcspn(entry->d_name, " \t\r\n");
        if (name_len == 0) {
            closedir(dir);
            return false;
        }
        if (n >= name_len) {
            n = name_len - 1;
        }
        memcpy(name, entry->d_name, n);
        name[n] = '\0';
        if (ifindex_out != NULL) {
            *ifindex_out = if_nametoindex(name);
        }
        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}

static bool set_ipv4_addr(const char *ifname, const struct in_addr *addr)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[init] set_ipv4 socket failed: %s\n", strerror(errno));
        return false;
    }

    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    sin->sin_addr = *addr;

    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        fprintf(stderr, "[init] set_ipv4 SIOCSIFADDR %s failed: %s\n",
                ifname, strerror(errno));
        close(fd);
        return false;
    }

    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        fprintf(stderr, "[init] set_ipv4 SIOCSIFNETMASK %s failed: %s\n",
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
    return addr->s_addr != 0;
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
        fprintf(stderr, "[init] static_arp socket failed: %s\n", strerror(errno));
        return;
    }

    if (ioctl(fd, SIOCSARP, &req) != 0) {
        fprintf(stderr, "[init] static_arp SIOCSARP %s failed: %s\n",
                ifname, strerror(errno));
    }

    close(fd);
}

static bool ipourma_role_ipv4_defaults(const char *role, char *local, size_t local_len,
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

static bool resolve_ipourma_ipv4_config(char *local, size_t local_len,
                                        char *peer, size_t peer_len,
                                        bool *have_peer)
{
    char role[32] = {0};
    bool have_local = cmdline_get_value("linqu_ipourma_ipv4", local, local_len);
    bool peer_present = cmdline_get_value("linqu_ipourma_peer_ipv4", peer, peer_len);

    if ((!have_local || !peer_present) &&
        cmdline_get_value("linqu_urma_dp_role", role, sizeof(role))) {
        char default_local[INET_ADDRSTRLEN];
        char default_peer[INET_ADDRSTRLEN];

        if (ipourma_role_ipv4_defaults(role, default_local, sizeof(default_local),
                                       default_peer, sizeof(default_peer))) {
            if (!have_local) {
                snprintf(local, local_len, "%s", default_local);
                have_local = true;
            }
            if (!peer_present) {
                snprintf(peer, peer_len, "%s", default_peer);
                peer_present = true;
            }
        }
    }

    if (!have_local) {
        if (have_peer != NULL) {
            *have_peer = false;
        }
        return false;
    }

    if (inet_pton(AF_INET, local, &(struct in_addr){0}) != 1) {
        fprintf(stderr, "[init] invalid linqu_ipourma_ipv4=%s\n", local);
        if (have_peer != NULL) {
            *have_peer = false;
        }
        return false;
    }

    if (peer_present && inet_pton(AF_INET, peer, &(struct in_addr){0}) != 1) {
        fprintf(stderr, "[init] invalid linqu_ipourma_peer_ipv4=%s\n", peer);
        peer_present = false;
    }

    if (have_peer != NULL) {
        *have_peer = peer_present;
    }
    return true;
}

static void dump_file(const char *path);

static void dump_ipourma_stats(void)
{
    DIR *dir = opendir("/sys/class/net");
    struct dirent *entry;
    char path[256];
    int written;

    if (!dir) {
        fprintf(stderr, "[init] opendir /sys/class/net failed: %s\n", strerror(errno));
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "ipourma", strlen("ipourma")) != 0) {
            continue;
        }
        written = snprintf(path, sizeof(path), "/sys/class/net/%s/query_ipourma_stats",
                           entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(path)) {
            fprintf(stderr, "[init] skip oversized ipourma stats path for %s\n",
                    entry->d_name);
            continue;
        }
        dump_file(path);
    }

    closedir(dir);
}

static void wait_for_ipourma_interface(int timeout_secs)
{
    int i;
    fprintf(stderr, "[init] waiting for ipourma network interface...\n");
    for (i = 0; i < timeout_secs * 2; i++) {
        if (is_ipourma_ready()) {
            fprintf(stderr, "[init] ipourma interface is UP, waiting for stabilization...\n");
            /* ADDITIONAL GRACE PERIOD: 5 seconds for stack stabilization */
            sleep(5);
            return;
        }
        usleep(500000); /* 500ms */
    }
    fprintf(stderr, "[init] TIMEOUT waiting for ipourma interface\n");
}

static bool configure_ipourma_network(int timeout_secs)
{
    char ifname[IFNAMSIZ] = {0};
    char local_ip[INET_ADDRSTRLEN] = {0};
    char peer_ip[INET_ADDRSTRLEN] = {0};
    struct in_addr desired_local = {0};
    struct in_addr current_local = {0};
    struct in_addr peer_addr = {0};
    unsigned int ifindex = 0;
    bool have_peer = false;

    if (!resolve_ipourma_ipv4_config(local_ip, sizeof(local_ip),
                                     peer_ip, sizeof(peer_ip),
                                     &have_peer)) {
        return false;
    }

    wait_for_ipourma_interface(timeout_secs);
    if (!find_ipourma_iface(ifname, sizeof(ifname), &ifindex)) {
        fprintf(stderr, "[init] ipourma bootstrap failed: interface not found\n");
        return false;
    }

    if (inet_pton(AF_INET, local_ip, &desired_local) != 1) {
        fprintf(stderr, "[init] ipourma bootstrap failed: local ip parse %s\n", local_ip);
        return false;
    }

    if (!set_ipv4_addr(ifname, &desired_local) || !get_local_ipv4(ifname, &current_local)) {
        fprintf(stderr, "[init] ipourma bootstrap failed: local ip apply %s on %s\n",
                local_ip, ifname);
        return false;
    }

    if (have_peer && inet_pton(AF_INET, peer_ip, &peer_addr) == 1) {
        install_static_arp(ifname, &peer_addr);
        fprintf(stderr, "[init] ipourma bootstrap iface=%s ifindex=%u local=%s peer=%s\n",
                ifname, ifindex, local_ip, peer_ip);
    } else {
        fprintf(stderr, "[init] ipourma bootstrap iface=%s ifindex=%u local=%s peer=(none)\n",
                ifname, ifindex, local_ip);
    }

    return true;
}

static void run_urma_dp_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for urma_dp failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_urma_dp", "/bin/linqu_urma_dp", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_urma_dp failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid urma_dp failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] urma dataplane pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] urma dataplane fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] urma dataplane fail signal=%d\n", WTERMSIG(status));
    } else {
        fprintf(stderr, "[init] urma dataplane fail unknown status=0x%x\n", status);
    }

    dump_ipourma_stats();
}

static bool wait_for_path(const char *path, int attempts, int sleep_ms);
static void try_insmod_module(const char *path, const char *module_name);

static void run_ub_chat_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_chat failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_chat", "/bin/linqu_ub_chat", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_chat failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_chat failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub chat pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub chat fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub chat fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_rdma_demo_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;

    /* Best-effort bootstrap; built-in vs module is not part of probe semantics. */
    try_insmod_module("/lib/modules/uburma.ko", "uburma");
    if (!wait_for_path("/dev/uburma", 30, 100) &&
        !wait_for_path("/sys/class/ubcore/udma0", 30, 100)) {
        fprintf(stderr, "[init] rdma interfaces not ready before demo start\n");
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_rdma_demo failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_rdma_demo", "/bin/linqu_ub_rdma_demo", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_rdma_demo failed: %s\n", strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ub_rdma_demo failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 60000) {
            fprintf(stderr, "[init] ub rdma demo timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub rdma demo pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub rdma demo fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub rdma demo fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub rdma demo fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_rpc_demo_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_rpc failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_rpc", "/bin/linqu_ub_rpc", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_rpc failed: %s\n", strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_rpc failed: %s\n", strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub rpc demo pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub rpc demo fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub rpc demo fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_tcp_each_server_demo_probe(void)
{
    pid_t pid;
    int status = 0;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_tcp_each_server failed: %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_tcp_each_server", "/bin/linqu_ub_tcp_each_server",
              (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_tcp_each_server failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "[init] waitpid ub_tcp_each_server failed: %s\n",
                strerror(errno));
        return;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub tcp each server demo pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub tcp each server demo fail exit=%d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub tcp each server demo fail signal=%d\n",
                WTERMSIG(status));
    }
}

static void run_obmm_demo_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;

    if (!wait_for_path("/dev/obmm", 50, 100) &&
        !wait_for_path("/sys/module/obmm", 50, 100)) {
        fprintf(stderr, "[init] obmm interfaces not ready before demo start\n");
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_obmm_demo failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_obmm_demo", "/bin/linqu_ub_obmm_demo", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_obmm_demo failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ub_obmm_demo failed: %s\n",
                    strerror(errno));
            return;
        }
        if (waited_ms >= 60000) {
            fprintf(stderr, "[init] ub obmm demo timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm demo pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm demo fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm demo fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm demo fail signal=%d\n", WTERMSIG(status));
    }
}

static void dump_dir_entries(const char *path)
{
    DIR *dir;
    struct dirent *ent;

    dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "[init] opendir %s failed: %s\n", path, strerror(errno));
        return;
    }

    fprintf(stderr, "[init] ls %s\n", path);
    while ((ent = readdir(dir)) != NULL) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) {
            continue;
        }
        fprintf(stderr, "[init]   %s\n", ent->d_name);
    }

    closedir(dir);
}

static void dump_file(const char *path)
{
    int fd;
    ssize_t n;
    char buf[512];

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[init] open %s failed: %s\n", path, strerror(errno));
        return;
    }

    fprintf(stderr, "[init] cat %s\n", path);
    while ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        fprintf(stderr, "%s", buf);
    }
    if (n < 0) {
        fprintf(stderr, "[init] read %s failed: %s\n", path, strerror(errno));
    }
    if (n > 0 && buf[n - 1] != '\n') {
        fprintf(stderr, "\n");
    }

    close(fd);
}

static void dump_ub_state(void)
{
    dump_dir_entries("/sys/bus");
    dump_dir_entries("/sys/bus/auxiliary");
    dump_dir_entries("/sys/bus/auxiliary/devices");
    dump_dir_entries("/sys/bus/auxiliary/drivers");
    dump_dir_entries("/sys/bus/platform/devices");
    dump_dir_entries("/sys/bus/platform/drivers");
    dump_dir_entries("/sys/bus/ub");
    dump_dir_entries("/sys/bus/ub/devices");
    dump_dir_entries("/sys/bus/ub/drivers");
    dump_dir_entries("/sys/bus/ub_service");
    dump_dir_entries("/sys/bus/ub_service/devices");
    dump_dir_entries("/sys/bus/ub_service/drivers");
    dump_dir_entries("/sys/bus/ub/devices/00001");
    dump_dir_entries("/sys/bus/ub/devices/00001/slot0");
    dump_dir_entries("/sys/bus/ub/devices/00001/port1");
    dump_dir_entries("/sys/bus/ub/devices/00002");
    dump_dir_entries("/sys/bus/ub/devices/00002/port0");
    dump_dir_entries("/sys/bus/ub_service/devices/00001:service002");
    dump_file("/sys/bus/ub/instance");
    dump_file("/sys/bus/ub/cluster");
    dump_file("/sys/bus/ub/devices/00001/slot0/power");
    dump_file("/sys/bus/ub/devices/00001/port1/boundary");
    dump_file("/sys/bus/ub/devices/00001/port1/linkup");
    dump_file("/sys/bus/ub/devices/00001/port1/cna");
    dump_file("/sys/bus/ub/devices/00001/port1/neighbor");
    dump_file("/sys/bus/ub/devices/00001/port1/neighbor_guid");
    dump_file("/sys/bus/ub/devices/00001/port1/neighbor_port_idx");
    dump_file("/sys/bus/ub/devices/00001/direct_link");
    dump_file("/sys/bus/ub/devices/00001/vendor");
    dump_file("/sys/bus/ub/devices/00001/device");
    dump_file("/sys/bus/ub/devices/00001/instance");
    dump_file("/sys/bus/ub/devices/00002/class_code");
    dump_file("/sys/bus/ub/devices/00002/type");
    dump_file("/sys/bus/ub/devices/00002/vendor");
    dump_file("/sys/bus/ub/devices/00002/device");
    dump_file("/sys/bus/ub/devices/00002/guid");
    dump_file("/sys/bus/ub/devices/00002/primary_entity");
    dump_file("/sys/bus/ub/devices/00002/instance");
    dump_file("/sys/bus/ub/devices/00002/resource");
    dump_file("/sys/bus/ub/devices/00002/port0/boundary");
    dump_file("/sys/bus/ub/devices/00002/port0/linkup");
    dump_file("/sys/bus/ub/devices/00002/port0/cna");
    dump_file("/sys/bus/ub/devices/00002/port0/neighbor");
    dump_file("/sys/bus/ub/devices/00002/port0/neighbor_guid");
    dump_file("/sys/bus/ub/devices/00002/port0/neighbor_port_idx");
    dump_file("/sys/bus/ub/devices/00002/direct_link");
    dump_dir_entries("/sys/class/net");
    dump_dir_entries("/sys/class/ubcore");
    dump_file("/proc/interrupts");
    dump_raw_ubc_port1_state();
}

static void dump_guest_payload_state(void)
{
    dump_dir_entries("/bin");
    dump_dir_entries("/lib");
    dump_dir_entries("/lib/modules");
}

static bool is_module_loaded(const char *module_name)
{
    char path[256];

    if (!module_name || !*module_name) {
        return false;
    }

    snprintf(path, sizeof(path), "/sys/module/%s", module_name);
    return access(path, F_OK) == 0;
}

static bool wait_for_path(const char *path, int attempts, int sleep_ms)
{
    int i;

    for (i = 0; i < attempts; i++) {
        if (access(path, F_OK) == 0) {
            return true;
        }
        usleep((useconds_t)sleep_ms * 1000);
    }

    return false;
}

static void try_insmod_module(const char *path, const char *module_name)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;

    if (is_module_loaded(module_name)) {
        fprintf(stderr, "[init] module %s already present, skip %s\n",
                module_name, path);
        return;
    }

    if (access("/bin/insmod", X_OK) != 0) {
        fprintf(stderr, "[init] insmod unavailable, skip bootstrap for %s: %s\n",
                module_name, strerror(errno));
        return;
    }

    if (access(path, R_OK) != 0) {
        fprintf(stderr, "[init] bootstrap module file absent for %s (%s), continue\n",
                module_name, path);
        return;
    }

    fprintf(stderr, "[init] bootstrap insmod %s via %s\n", module_name, path);
    pid = fork();
    if (pid == 0) {
        execl("/bin/insmod", "/bin/insmod", path, (char *)NULL);
        _exit(127);
    }
    if (pid < 0) {
        fprintf(stderr, "[init] fork for insmod failed: %s\n", strerror(errno));
        return;
    }

    while (waitpid(pid, &status, WNOHANG) == 0) {
        if (waited_ms >= 4000) {
            fprintf(stderr, "[init] insmod timeout %s, killing pid=%d\n", path, pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] bootstrap insmod %s exit=%d\n",
                module_name, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] bootstrap insmod %s signal=%d\n",
                module_name, WTERMSIG(status));
    }
}

static void bootstrap_drivers(void)
{
    /*
     * Best-effort bootstrap only.
     * Harness semantics must bind to interfaces and functionality, not module form.
     */
    try_insmod_module("/lib/modules/ubus.ko", "ubus");
    try_insmod_module("/lib/modules/ummu-core.ko", "ummu_core");
    try_insmod_module("/lib/modules/ummu.ko", "ummu");
    try_insmod_module("/lib/modules/ubase.ko", "ubase");
    try_insmod_module("/lib/modules/hisi_ubus.ko", "hisi_ubus");
    try_insmod_module("/lib/modules/obmm.ko", "obmm");
    try_insmod_module("/lib/modules/ub-sim-decoder.ko", "ub_sim_decoder");
    try_insmod_module("/lib/modules/ubcore.ko", "ubcore");
    try_insmod_module("/lib/modules/udma.ko", "udma");
    try_insmod_module("/lib/modules/ipourma.ko", "ipourma");
    if (cmdline_has_option("linqu_probe_load_helper=1")) {
        try_insmod_module("/lib/modules/linqu_ub_drv.ko", "linqu_ub_drv");
    }
}

static bool wait_for_ub_sysfs_ready(void)
{
    static const char *required_paths[] = {
        "/sys/bus/ub/devices/00001/port1/linkup",
        "/sys/bus/ub/devices/00001",
    };
    int attempt;
    size_t i;

    for (attempt = 0; attempt < 60; attempt++) {
        bool all_ready = true;

        for (i = 0; i < sizeof(required_paths) / sizeof(required_paths[0]); i++) {
            if (access(required_paths[i], F_OK) != 0) {
                all_ready = false;
                break;
            }
        }
        if (all_ready) {
            fprintf(stderr, "[init] ub sysfs ready via %s\n", required_paths[0]);
            return true;
        }
        usleep(100000);
    }

    fprintf(stderr, "[init] ub sysfs wait timed out\n");
    return false;
}

static void write_sysfs_text(const char *path, const char *text)
{
    int fd;
    size_t len;
    ssize_t n;

    fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "[init] open %s failed: %s\n", path, strerror(errno));
        return;
    }

    len = strlen(text);
    n = write(fd, text, len);
    if (n != (ssize_t)len) {
        fprintf(stderr, "[init] write %s failed: %s\n", path,
                (n < 0) ? strerror(errno) : "short write");
    } else {
        fprintf(stderr, "[init] write %s <= %s", path, text);
    }
    close(fd);
}

static void force_bind_ubase_for_qemu(void)
{
    static const char *devs[] = {"00001"};
    size_t i;
    int attempt;
    const int max_attempts = 60;
    char path[256];
    char text[32];
    char driver_link[256];
    char link_target[256];
    ssize_t n;
    bool bound = false;

    if (!cmdline_has_option("linqu_force_ubase_bind=1")) {
        return;
    }

    for (i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        bound = false;
        for (attempt = 0; attempt < max_attempts; attempt++) {
            snprintf(driver_link, sizeof(driver_link),
                     "/sys/bus/ub/devices/%s/driver", devs[i]);
            n = readlink(driver_link, link_target, sizeof(link_target) - 1);
            if (n > 0) {
                const char *base = NULL;

                link_target[n] = '\0';
                base = strrchr(link_target, '/');
                if (!base) {
                    base = link_target;
                } else {
                    base++;
                }
                if (!strcmp(base, "ubase")) {
                    fprintf(stderr, "[init] %s already bound to ubase\n", devs[i]);
                    bound = true;
                    break;
                }
            }

            snprintf(path, sizeof(path), "/sys/bus/ub/devices/%s/driver_override",
                     devs[i]);
            write_sysfs_text(path, "ubase\n");

            snprintf(path, sizeof(path), "/sys/bus/ub/drivers/ub_generic_component/unbind");
            snprintf(text, sizeof(text), "%s\n", devs[i]);
            write_sysfs_text(path, text);

            snprintf(path, sizeof(path), "/sys/bus/ub/drivers_probe");
            write_sysfs_text(path, text);
            usleep(500000);
        }

        if (!bound) {
            fprintf(stderr, "[init] warn: %s still not bound to ubase after %d attempts\n",
                    devs[i], max_attempts);
        }
    }
}

int main(void)
{
    puts("[init] linqu-ub linux probe");

    if (setsid() < 0) {
        fprintf(stderr, "[init] setsid failed: %s\n", strerror(errno));
    }

    ensure_dir("/proc");
    ensure_dir("/sys");
    ensure_dir("/dev");
    ensure_dir("/dev/pts");
    ensure_dir("/dev/shm");
    ensure_dir("/tmp");

    try_mount("none", "/proc", "proc", 0);
    try_mount("none", "/sys", "sysfs", 0);
    try_mount("none", "/dev", "devtmpfs", 0);
    try_mount("none", "/dev/pts", "devpts", 0);

    dump_ub_state();
    dump_guest_payload_state();
    bootstrap_drivers();
    fprintf(stderr, "[init] bootstrap complete, entering wait_for_ub_sysfs_ready\n");
    (void)wait_for_ub_sysfs_ready();
    fprintf(stderr, "[init] wait_for_ub_sysfs_ready returned\n");
    dump_ub_state();
    if (should_run_bizmsg_verify()) {
        run_bizmsg_roundtrip_probe();
    }
    force_bind_ubase_for_qemu();
    dump_ub_state();
    (void)configure_ipourma_network(30);
    if (should_run_urma_dp_verify()) {
        /* Wait up to 30 seconds for asynchronous device registration to complete */
        wait_for_ipourma_interface(30);
        run_urma_dp_probe();
    }
    if (should_run_ub_chat()) {
        wait_for_ipourma_interface(30);
        run_ub_chat_probe();
    }
    if (should_run_ub_rpc_demo()) {
        wait_for_ipourma_interface(30);
        run_ub_rpc_demo_probe();
    }
    if (should_run_ub_tcp_each_server_demo()) {
        wait_for_ipourma_interface(30);
        run_ub_tcp_each_server_demo_probe();
    }
    if (should_run_ub_rdma_demo()) {
        wait_for_ipourma_interface(30);
        run_ub_rdma_demo_probe();
    }
    if (should_run_obmm_demo()) {
        wait_for_ipourma_interface(30);
        run_obmm_demo_probe();
    }
    if (should_run_linqu_probe()) {
        run_probe();
    } else {
        fprintf(stderr, "[init] linqu_probe skipped by cmdline\n");
    }
    dump_ub_state();

    if (should_enter_demo_boot_flow() && access("/bin/run_demo", X_OK) == 0) {
        fprintf(stderr, "[init] switching to /bin/run_demo boot flow\n");
        execl("/bin/run_demo", "/bin/run_demo", "--resume", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/run_demo failed: %s\n", strerror(errno));
    }

    if (should_hold_after_probe()) {
        fprintf(stderr, "[init] holding after probe by cmdline\n");
        for (;;) {
            pause();
        }
    }

    puts("[init] probe finished, powering off");
    sync();
    reboot(RB_POWER_OFF);
    reboot(RB_AUTOBOOT);

    for (;;) {
        pause();
    }
}
