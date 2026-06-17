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

static bool should_enter_app_boot_flow(void);

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

static bool should_run_ub_udma(void)
{
    return cmdline_has_option("linqu_ub_udma=1");
}

static bool should_run_ub_tcp_each_server(void)
{
    return cmdline_has_option("linqu_ub_tcp_each_server=1");
}

static bool should_run_obmm_pool(void)
{
    return cmdline_has_option("linqu_obmm_pool=1");
}

static bool should_run_obmm_queue(void)
{
    return cmdline_has_option("linqu_obmm_queue=1");
}

static bool should_run_obmm_dataplane_microbench(void)
{
    return cmdline_has_option("linqu_obmm_dataplane_microbench=1");
}

static bool should_run_obmm_import_stress(void)
{
    return cmdline_has_option("linqu_obmm_import_stress=1");
}

static bool should_run_obmm_gsva(void)
{
    return cmdline_has_option("linqu_obmm_gsva=1");
}

static bool should_run_obmm_coh_test(void)
{
    return cmdline_has_option("linqu_obmm_coh_test=1");
}

static bool should_run_gsva_query(void)
{
    return cmdline_has_option("linqu_gsva_query=1");
}

static bool should_run_gsva_coh_test(void)
{
    return cmdline_has_option("linqu_gsva_coh_test=1");
}

static bool should_run_gsva_lifecycle_test(void)
{
    return cmdline_has_option("linqu_gsva_lifecycle_test=1");
}

static bool should_run_npu_test(void)
{
    return cmdline_has_option("linqu_npu_test=1");
}

static bool should_run_npu_gsva_test(void)
{
    return cmdline_has_option("linqu_npu_gsva_test=1");
}

static bool should_run_ssd_test(void)
{
    return cmdline_has_option("linqu_ssd_test=1");
}

static bool should_run_ssd_gsva_test(void)
{
    return cmdline_has_option("linqu_ssd_gsva_test=1");
}

static bool should_run_gva_direct(void)
{
    return cmdline_has_option("linqu_gva_direct=1");
}

static bool should_enter_app_boot_flow(void)
{
    const char *flag = getenv("UB_RUN_APP_FROM_INIT");
    return flag != NULL && strcmp(flag, "1") == 0;
}

static bool should_run_ub_rpc(void)
{
    return cmdline_has_option("linqu_ub_rpc=1");
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
    if (strcmp(role, "nodeA") == 0 ||
        strcmp(role, "initiator") == 0 ||
        strcmp(role, "client") == 0 ||
        strcmp(role, "exporter") == 0) {
        snprintf(local, local_len, "%s", "10.0.0.1");
        snprintf(peer, peer_len, "%s", "10.0.0.2");
        return true;
    }
    if (strcmp(role, "nodeB") == 0 ||
        strcmp(role, "responder") == 0 ||
        strcmp(role, "server") == 0 ||
        strcmp(role, "importer") == 0) {
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

static void run_ub_udma_probe(void)
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
        fprintf(stderr, "[init] udma interfaces not ready before app start\n");
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_udma failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_udma", "/bin/linqu_ub_udma", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_udma failed: %s\n", strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ub_udma failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 60000) {
            fprintf(stderr, "[init] ub udma app timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub udma app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub udma app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub udma app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub udma app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_rpc_probe(void)
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
        fprintf(stderr, "[init] ub rpc app pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub rpc app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub rpc app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ub_tcp_each_server_probe(void)
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
        fprintf(stderr, "[init] ub tcp each server app pass\n");
        return;
    }

    if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub tcp each server app fail exit=%d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub tcp each server app fail signal=%d\n",
                WTERMSIG(status));
    }
}

static void run_obmm_pool_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char obmm_pool_local_ip[64] = "";
    char obmm_pool_all_ips[256] = "";
    char obmm_pool_node_count[16] = "";
    char obmm_pool_export_size_mb[64] = "";
    char obmm_pool_import_cache_mode[64] = "";
    char obmm_pool_stress_iters[64] = "";
    char obmm_pool_round_timeout_ms[64] = "";

    if (!wait_for_path("/dev/obmm", 50, 100) &&
        !wait_for_path("/sys/module/obmm", 50, 100)) {
        fprintf(stderr, "[init] obmm interfaces not ready before pool app start\n");
    }

    if (cmdline_get_value("obmm_pool_local_ip", obmm_pool_local_ip,
                          sizeof(obmm_pool_local_ip))) {
        setenv("LINQU_UB_LOCAL_IP", obmm_pool_local_ip, 1);
    }
    if (cmdline_get_value("obmm_pool_all_ips", obmm_pool_all_ips,
                          sizeof(obmm_pool_all_ips))) {
        setenv("LINQU_UB_ALL_IPS", obmm_pool_all_ips, 1);
    }
    if (cmdline_get_value("obmm_pool_node_count", obmm_pool_node_count,
                          sizeof(obmm_pool_node_count))) {
        setenv("LINQU_UB_NODE_COUNT", obmm_pool_node_count, 1);
    }
    if (cmdline_get_value("obmm_pool_export_size_mb", obmm_pool_export_size_mb,
                          sizeof(obmm_pool_export_size_mb))) {
        setenv("OBMM_POOL_EXPORT_SIZE_MB", obmm_pool_export_size_mb, 1);
    }
    if (cmdline_get_value("obmm_pool_import_cache_mode", obmm_pool_import_cache_mode,
                          sizeof(obmm_pool_import_cache_mode))) {
        setenv("OBMM_IMPORT_CACHE_MODE", obmm_pool_import_cache_mode, 1);
    }
    if (cmdline_get_value("obmm_pool_stress_iters", obmm_pool_stress_iters,
                          sizeof(obmm_pool_stress_iters))) {
        setenv("OBMM_POOL_STRESS_ITERS", obmm_pool_stress_iters, 1);
    }
    if (cmdline_get_value("obmm_pool_round_timeout_ms", obmm_pool_round_timeout_ms,
                          sizeof(obmm_pool_round_timeout_ms))) {
        setenv("OBMM_POOL_ROUND_TIMEOUT_MS", obmm_pool_round_timeout_ms, 1);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ub_obmm_pool failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execl("/bin/linqu_ub_obmm_pool", "/bin/linqu_ub_obmm_pool", (char *)NULL);
        fprintf(stderr, "[init] exec /bin/linqu_ub_obmm_pool failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ub_obmm_pool failed: %s\n",
                    strerror(errno));
            return;
        }
        if (waited_ms >= 60000) {
            fprintf(stderr, "[init] ub obmm pool app timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm pool app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm pool app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm pool app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm pool app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_obmm_queue_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char obmm_queue_local_ip[64] = "";
    char obmm_queue_all_ips[128] = "";
    char obmm_queue_node_count[16] = "";
    char obmm_queue_export_size_mb[32] = "";
    char obmm_queue_depth[32] = "";
    char obmm_queue_bootstrap[16] = "";
    char obmm_queue_bootstrap_session[64] = "";
    char obmm_queue_mode[32] = "";
    char obmm_queue_import_cache_mode[32] = "";

    if (!wait_for_path("/dev/obmm", 50, 100) &&
        !wait_for_path("/sys/module/obmm", 50, 100)) {
        fprintf(stderr, "[init] obmm interfaces not ready before queue app start\n");
    }

    if (cmdline_get_value("obmm_queue_local_ip", obmm_queue_local_ip,
                          sizeof(obmm_queue_local_ip))) {
        setenv("LINQU_UB_LOCAL_IP", obmm_queue_local_ip, 1);
    }
    if (cmdline_get_value("obmm_queue_all_ips", obmm_queue_all_ips,
                          sizeof(obmm_queue_all_ips))) {
        setenv("LINQU_UB_ALL_IPS", obmm_queue_all_ips, 1);
    }
    if (cmdline_get_value("obmm_queue_node_count", obmm_queue_node_count,
                          sizeof(obmm_queue_node_count))) {
        setenv("LINQU_UB_NODE_COUNT", obmm_queue_node_count, 1);
    }
    if (cmdline_get_value("obmm_queue_export_size_mb", obmm_queue_export_size_mb,
                          sizeof(obmm_queue_export_size_mb))) {
        setenv("OBMM_POOL_EXPORT_SIZE_MB", obmm_queue_export_size_mb, 1);
    }
    if (cmdline_get_value("obmm_queue_depth", obmm_queue_depth,
                          sizeof(obmm_queue_depth))) {
        setenv("OBMM_QUEUE_DEPTH", obmm_queue_depth, 1);
    }
    if (cmdline_get_value("obmm_queue_bootstrap", obmm_queue_bootstrap,
                          sizeof(obmm_queue_bootstrap))) {
        setenv("OBMM_BOOTSTRAP", obmm_queue_bootstrap, 1);
    }
    if (cmdline_get_value("obmm_queue_bootstrap_session", obmm_queue_bootstrap_session,
                          sizeof(obmm_queue_bootstrap_session))) {
        setenv("OBMM_BOOTSTRAP_SESSION", obmm_queue_bootstrap_session, 1);
    }
    if (cmdline_get_value("obmm_queue_mode", obmm_queue_mode,
                          sizeof(obmm_queue_mode))) {
        setenv("OBMM_QUEUE_MODE", obmm_queue_mode, 1);
    }
    if (cmdline_get_value("obmm_queue_import_cache_mode", obmm_queue_import_cache_mode,
                          sizeof(obmm_queue_import_cache_mode))) {
        setenv("OBMM_IMPORT_CACHE_MODE", obmm_queue_import_cache_mode, 1);
    }

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for obmm queue app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        char *argv[] = {"/bin/linqu_ub_obmm_queue", (char *)NULL};
        execv("/bin/linqu_ub_obmm_queue", argv);
        fprintf(stderr, "[init] exec /bin/linqu_ub_obmm_queue failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid obmm queue app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 180000) {
            fprintf(stderr, "[init] ub obmm queue app timeout, killing pid=%d\n",
                    pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm queue app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm queue app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm queue app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm queue app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_obmm_dataplane_microbench_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char dp_mode[64] = "";
    char dp_size[64] = "";
    char dp_iters[64] = "";
    char dp_chunk_size[64] = "";
    char dp_generic_pte_offset[64] = "";
    char dp_gsva_base[64] = "";
    char dp_gsva_generation[64] = "";
    char *argv[20];
    int argc = 0;

    argv[argc++] = "/bin/linqu_ub_obmm_dataplane_microbench";
    if (cmdline_get_value("obmm_dp_mode", dp_mode, sizeof(dp_mode))) {
        argv[argc++] = "--mode";
        argv[argc++] = dp_mode;
    }
    if (cmdline_get_value("obmm_dp_size", dp_size, sizeof(dp_size))) {
        argv[argc++] = "--size";
        argv[argc++] = dp_size;
    }
    if (cmdline_get_value("obmm_dp_iters", dp_iters, sizeof(dp_iters))) {
        argv[argc++] = "--iterations";
        argv[argc++] = dp_iters;
    }
    if (cmdline_get_value("obmm_dp_chunk_size", dp_chunk_size,
                          sizeof(dp_chunk_size))) {
        argv[argc++] = "--chunk-size";
        argv[argc++] = dp_chunk_size;
    }
    if (cmdline_get_value("obmm_dp_generic_pte_offset",
                          dp_generic_pte_offset,
                          sizeof(dp_generic_pte_offset))) {
        argv[argc++] = "--generic-pte-offset";
        argv[argc++] = dp_generic_pte_offset;
    }
    if (cmdline_get_value("obmm_dp_gsva_base", dp_gsva_base,
                          sizeof(dp_gsva_base))) {
        argv[argc++] = "--gsva-base";
        argv[argc++] = dp_gsva_base;
    }
    if (cmdline_get_value("obmm_dp_gsva_generation",
                          dp_gsva_generation,
                          sizeof(dp_gsva_generation))) {
        argv[argc++] = "--gsva-generation";
        argv[argc++] = dp_gsva_generation;
    }
    if (cmdline_has_option("obmm_dp_verify=1")) {
        argv[argc++] = "--verify";
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for obmm dataplane microbench failed: %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_obmm_dataplane_microbench", argv);
        fprintf(stderr,
                "[init] exec /bin/linqu_ub_obmm_dataplane_microbench failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr,
                    "[init] waitpid obmm dataplane microbench failed: %s\n",
                    strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr,
                    "[init] ub obmm dataplane microbench app timeout, killing pid=%d\n",
                    pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr,
                "[init] ub obmm dataplane microbench app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr,
                "[init] ub obmm dataplane microbench app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm dataplane microbench app fail exit=%d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr,
                "[init] ub obmm dataplane microbench app fail signal=%d\n",
                WTERMSIG(status));
    }
}

static void run_obmm_import_stress_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char stress_size[64] = "";
    char stress_pattern[64] = "";
    char stress_iters[64] = "";
    char stress_flush[64] = "";
    char stress_period[64] = "";
    char stress_chunk_size[64] = "";
    char stress_seed[64] = "";
    char stress_gva_mode[64] = "";
    char stress_gva_map_source[64] = "";
    char stress_gva_address_profile[64] = "";
    char stress_gva_cache_policy[64] = "";
    char stress_gva_vmid[64] = "";
    char stress_gva_asid[64] = "";
    char stress_gva_tid[64] = "";
    char stress_gva_p_tag[64] = "";
    char stress_gva_access_flags[64] = "";
    char stress_gva_token_value[64] = "";
    char stress_gva_id[64] = "";
    char stress_gva_user_va[64] = "";
    char stress_gva_home_va[64] = "";
    char stress_gva_pte_offset[64] = "";
    char stress_gsva_base[64] = "";
    char stress_gsva_generation[64] = "";
    char *argv[48];
    int argc = 0;

    argv[argc++] = "/bin/linqu_ub_obmm_import_stress";
    if (cmdline_get_value("obmm_stress_size", stress_size, sizeof(stress_size))) {
        argv[argc++] = "--size";
        argv[argc++] = stress_size;
    }
    if (cmdline_get_value("obmm_stress_pattern", stress_pattern, sizeof(stress_pattern))) {
        argv[argc++] = "--pattern";
        argv[argc++] = stress_pattern;
    }
    if (cmdline_get_value("obmm_stress_iters", stress_iters, sizeof(stress_iters))) {
        argv[argc++] = "--iterations";
        argv[argc++] = stress_iters;
    }
    if (cmdline_get_value("obmm_stress_flush", stress_flush, sizeof(stress_flush))) {
        argv[argc++] = "--flush-mode";
        argv[argc++] = stress_flush;
    }
    if (cmdline_get_value("obmm_stress_period", stress_period, sizeof(stress_period))) {
        argv[argc++] = "--period";
        argv[argc++] = stress_period;
    }
    if (cmdline_get_value("obmm_stress_chunk_size", stress_chunk_size,
                          sizeof(stress_chunk_size))) {
        argv[argc++] = "--chunk-size";
        argv[argc++] = stress_chunk_size;
    }
    if (cmdline_get_value("obmm_stress_seed", stress_seed, sizeof(stress_seed))) {
        argv[argc++] = "--seed";
        argv[argc++] = stress_seed;
    }
    if (cmdline_get_value("obmm_stress_gva_mode", stress_gva_mode, sizeof(stress_gva_mode))) {
        argv[argc++] = "--gva-mode";
        argv[argc++] = stress_gva_mode;
    }
    if (cmdline_get_value("obmm_stress_gva_map_source", stress_gva_map_source,
                          sizeof(stress_gva_map_source))) {
        argv[argc++] = "--gva-map-source";
        argv[argc++] = stress_gva_map_source;
    }
    if (cmdline_get_value("obmm_stress_gva_address_profile",
                          stress_gva_address_profile,
                          sizeof(stress_gva_address_profile))) {
        argv[argc++] = "--gva-address-profile";
        argv[argc++] = stress_gva_address_profile;
    }
    if (cmdline_get_value("obmm_stress_gva_cache_policy", stress_gva_cache_policy,
                          sizeof(stress_gva_cache_policy))) {
        argv[argc++] = "--gva-cache-policy";
        argv[argc++] = stress_gva_cache_policy;
    }
    if (cmdline_get_value("obmm_stress_gva_vmid", stress_gva_vmid, sizeof(stress_gva_vmid))) {
        argv[argc++] = "--gva-vmid";
        argv[argc++] = stress_gva_vmid;
    }
    if (cmdline_get_value("obmm_stress_gva_asid", stress_gva_asid, sizeof(stress_gva_asid))) {
        argv[argc++] = "--gva-asid";
        argv[argc++] = stress_gva_asid;
    }
    if (cmdline_get_value("obmm_stress_gva_tid", stress_gva_tid, sizeof(stress_gva_tid))) {
        argv[argc++] = "--gva-tid";
        argv[argc++] = stress_gva_tid;
    }
    if (cmdline_get_value("obmm_stress_gva_p_tag", stress_gva_p_tag, sizeof(stress_gva_p_tag))) {
        argv[argc++] = "--gva-p-tag";
        argv[argc++] = stress_gva_p_tag;
    }
    if (cmdline_get_value("obmm_stress_gva_access_flags", stress_gva_access_flags,
                          sizeof(stress_gva_access_flags))) {
        argv[argc++] = "--gva-access-flags";
        argv[argc++] = stress_gva_access_flags;
    }
    if (cmdline_get_value("obmm_stress_gva_token_value", stress_gva_token_value,
                          sizeof(stress_gva_token_value))) {
        argv[argc++] = "--gva-token-value";
        argv[argc++] = stress_gva_token_value;
    }
    if (cmdline_get_value("obmm_stress_gva_id", stress_gva_id, sizeof(stress_gva_id))) {
        argv[argc++] = "--gva-id";
        argv[argc++] = stress_gva_id;
    }
    if (cmdline_get_value("obmm_stress_gva_user_va", stress_gva_user_va,
                          sizeof(stress_gva_user_va))) {
        argv[argc++] = "--gva-user-va";
        argv[argc++] = stress_gva_user_va;
    }
    if (cmdline_get_value("obmm_stress_gva_home_va", stress_gva_home_va,
                          sizeof(stress_gva_home_va))) {
        argv[argc++] = "--gva-home-va";
        argv[argc++] = stress_gva_home_va;
    }
    if (cmdline_get_value("obmm_stress_gva_pte_offset", stress_gva_pte_offset,
                          sizeof(stress_gva_pte_offset))) {
        argv[argc++] = "--gva-pte-offset";
        argv[argc++] = stress_gva_pte_offset;
    }
    if (cmdline_get_value("obmm_stress_gsva_base", stress_gsva_base, sizeof(stress_gsva_base))) {
        argv[argc++] = "--gsva-base";
        argv[argc++] = stress_gsva_base;
    }
    if (cmdline_get_value("obmm_stress_gsva_generation", stress_gsva_generation,
                          sizeof(stress_gsva_generation))) {
        argv[argc++] = "--gsva-generation";
        argv[argc++] = stress_gsva_generation;
    }
    if (cmdline_has_option("obmm_stress_verify=1")) {
        argv[argc++] = "--verify";
    }
    if (cmdline_has_option("obmm_stress_read_only=1")) {
        argv[argc++] = "--read-only";
    }
    if (cmdline_has_option("obmm_stress_write_only=1")) {
        argv[argc++] = "--write-only";
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for obmm import stress failed: %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_obmm_import_stress", argv);
        fprintf(stderr, "[init] exec /bin/linqu_ub_obmm_import_stress failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid obmm import stress failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 180000) {
            fprintf(stderr,
                    "[init] ub obmm import stress app timeout, killing pid=%d\n",
                    pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm import stress app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm import stress app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm import stress app fail exit=%d\n",
                WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm import stress app fail signal=%d\n",
                WTERMSIG(status));
    }
}

static void run_obmm_gsva_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char gsva_mode[64] = "";
    char gsva_base[64] = "";
    char gsva_size[64] = "";
    char gsva_node_count[64] = "";
    char *argv[12];
    int argc = 0;

    argv[argc++] = "/bin/linqu_ub_obmm_gsva";
    if (!cmdline_get_value("obmm_gsva_mode", gsva_mode, sizeof(gsva_mode)) &&
        !cmdline_get_value("OBMM_GSVA_MODE", gsva_mode, sizeof(gsva_mode))) {
        gsva_mode[0] = '\0';
    } else {
        argv[argc++] = "--mode";
        argv[argc++] = gsva_mode;
    }

    if (!cmdline_get_value("obmm_gsva_base", gsva_base, sizeof(gsva_base)) &&
        !cmdline_get_value("OBMM_GSVA_BASE", gsva_base, sizeof(gsva_base))) {
        gsva_base[0] = '\0';
    } else {
        argv[argc++] = "--base";
        argv[argc++] = gsva_base;
    }

    if (!cmdline_get_value("obmm_gsva_size", gsva_size, sizeof(gsva_size)) &&
        !cmdline_get_value("OBMM_GSVA_SIZE", gsva_size, sizeof(gsva_size))) {
        gsva_size[0] = '\0';
    } else {
        argv[argc++] = "--size";
        argv[argc++] = gsva_size;
    }

    if (!cmdline_get_value("obmm_gsva_node_count", gsva_node_count,
                          sizeof(gsva_node_count)) &&
        !cmdline_get_value("OBMM_GSVA_NODE_COUNT", gsva_node_count,
                          sizeof(gsva_node_count))) {
        gsva_node_count[0] = '\0';
    } else {
        argv[argc++] = "--node-count";
        argv[argc++] = gsva_node_count;
    }

    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for obmm gsva failed: %s\n",
                strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_obmm_gsva", argv);
        fprintf(stderr,
                "[init] exec /bin/linqu_ub_obmm_gsva failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid obmm gsva failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr,
                    "[init] ub obmm gsva app timeout, killing pid=%d\n",
                    pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm gsva app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm gsva app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm gsva app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm gsva app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_npu_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char *argv[2];
    int argc = 0;

    argv[argc++] = "/bin/npu_test";
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for npu test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/npu_test", argv);
        fprintf(stderr,
                "[init] exec /bin/npu_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid npu test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub npu test app timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub npu test app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub npu test app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub npu test app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub npu test app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_gsva_coh_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char gsva_test_mode[64] = "all";
    char *argv[4];
    int argc = 0;

    cmdline_get_value("gsva_test_mode", gsva_test_mode, sizeof(gsva_test_mode));

    argv[argc++] = "/bin/linqu_ub_gsva_coh_test";
    argv[argc++] = "--mode";
    argv[argc++] = gsva_test_mode;
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for gsva coh test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_gsva_coh_test", argv);
        fprintf(stderr, "[init] exec /bin/linqu_ub_gsva_coh_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid gsva coh test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub gsva coh app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub gsva coh app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub gsva coh app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub gsva coh app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub gsva coh app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_gsva_lifecycle_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char gsva_test_mode[64] = "all";
    char *argv[4];
    int argc = 0;

    cmdline_get_value("gsva_test_mode", gsva_test_mode, sizeof(gsva_test_mode));

    argv[argc++] = "/bin/linqu_ub_gsva_lifecycle_test";
    argv[argc++] = "--mode";
    argv[argc++] = gsva_test_mode;
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for gsva lifecycle test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_gsva_lifecycle_test", argv);
        fprintf(stderr, "[init] exec /bin/linqu_ub_gsva_lifecycle_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid gsva lifecycle test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub gsva lifecycle app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub gsva lifecycle app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub gsva lifecycle app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub gsva lifecycle app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub gsva lifecycle app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_npu_gsva_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char *argv[] = {"/bin/npu_gsva_test", (char *)NULL};

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for npu gsva test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/npu_gsva_test", argv);
        fprintf(stderr,
                "[init] exec /bin/npu_gsva_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid npu gsva test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub npu gsva app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub npu gsva app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub npu gsva app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub npu gsva app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub npu gsva app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ssd_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char *argv[] = {"/bin/ssd_test", (char *)NULL};

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ssd test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/ssd_test", argv);
        fprintf(stderr,
                "[init] exec /bin/ssd_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ssd test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub ssd test app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub ssd test app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub ssd test app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub ssd test app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub ssd test app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_ssd_gsva_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char *argv[] = {"/bin/ssd_gsva_test", (char *)NULL};

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for ssd gsva test app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/ssd_gsva_test", argv);
        fprintf(stderr,
                "[init] exec /bin/ssd_gsva_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid ssd gsva test app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub ssd gsva app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub ssd gsva app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub ssd gsva app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub ssd gsva app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub ssd gsva app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_gsva_query_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char gsva_query_mode[64] = "caps";
    char gsva_query_segment_id[64] = "";
    char gsva_query_mode_opt[80] = "";
    char *argv[8];
    int argc = 0;
    char *mode_ptr;

    if (cmdline_get_value("gsva_query_mode", gsva_query_mode, sizeof(gsva_query_mode))) {
        mode_ptr = gsva_query_mode;
    } else {
        mode_ptr = "caps";
    }

    if (strncmp(mode_ptr, "--", 2) == 0) {
        snprintf(gsva_query_mode_opt, sizeof(gsva_query_mode_opt), "%s", mode_ptr);
    } else {
        snprintf(gsva_query_mode_opt, sizeof(gsva_query_mode_opt), "--%s", mode_ptr);
    }

    if (cmdline_get_value("segment-id", gsva_query_segment_id, sizeof(gsva_query_segment_id))) {
        /* no-op */
    } else if (cmdline_get_value("gsva_query_segment_id",
                                 gsva_query_segment_id,
                                 sizeof(gsva_query_segment_id))) {
        /* no-op */
    }

    argv[argc++] = "/bin/linqu_ub_gsva_query";
    argv[argc++] = gsva_query_mode_opt;
    if (strlen(gsva_query_segment_id) > 0) {
        argv[argc++] = "--segment-id";
        argv[argc++] = gsva_query_segment_id;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for gsva query app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_gsva_query", argv);
        fprintf(stderr,
                "[init] exec /bin/linqu_ub_gsva_query failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid gsva query app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub gsva query app fail timeout\n");
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub gsva query app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub gsva query app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub gsva query app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub gsva query app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_gva_direct_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char gva_direct_mode[64] = "";
    char gva_direct_size[64] = "";
    char gva_direct_local_va[64] = "";
    char gva_direct_home_va[64] = "";
    char *argv[16];
    int argc = 0;

    argv[argc++] = "/bin/linqu_gva_direct";
    if (cmdline_get_value("gva_direct_mode", gva_direct_mode, sizeof(gva_direct_mode))) {
        argv[argc++] = "--mode";
        argv[argc++] = gva_direct_mode;
    }
    if (cmdline_get_value("gva_direct_size", gva_direct_size, sizeof(gva_direct_size))) {
        argv[argc++] = "--size";
        argv[argc++] = gva_direct_size;
    }
    if (cmdline_get_value("gva_direct_local_va",
                          gva_direct_local_va,
                          sizeof(gva_direct_local_va))) {
        argv[argc++] = "--local-va";
        argv[argc++] = gva_direct_local_va;
    }
    if (cmdline_get_value("gva_direct_home_va", gva_direct_home_va, sizeof(gva_direct_home_va))) {
        argv[argc++] = "--home-va";
        argv[argc++] = gva_direct_home_va;
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for gva direct app failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_gva_direct", argv);
        fprintf(stderr, "[init] exec /bin/linqu_gva_direct failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid gva direct app failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub gva direct app timeout, killing pid=%d\n", pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub gva direct app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub gva direct app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub gva direct app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub gva direct app fail signal=%d\n", WTERMSIG(status));
    }
}

static void run_obmm_coh_test_probe(void)
{
    pid_t pid;
    int status = 0;
    int waited_ms = 0;
    bool timed_out = false;
    pid_t wait_ret;
    char coh_mode[64] = "";
    char coh_size[64] = "";
    char coh_iters[64] = "";
    char coh_node_id[64] = "";
    char coh_node_count[64] = "";
    char coh_token_value[64] = "";
    char coh_generation[64] = "";
    char *argv[24];
    int argc = 0;

    argv[argc++] = "/bin/linqu_ub_obmm_coh_test";
    if (cmdline_get_value("obmm_coh_test_mode", coh_mode, sizeof(coh_mode))) {
        argv[argc++] = "--mode";
        argv[argc++] = coh_mode;
    }
    if (cmdline_get_value("obmm_coh_test_size", coh_size, sizeof(coh_size))) {
        argv[argc++] = "--size";
        argv[argc++] = coh_size;
    }
    if (cmdline_get_value("obmm_coh_test_iters", coh_iters, sizeof(coh_iters))) {
        argv[argc++] = "--iterations";
        argv[argc++] = coh_iters;
    }
    if (cmdline_get_value("obmm_coh_test_node_id", coh_node_id, sizeof(coh_node_id))) {
        argv[argc++] = "--node-id";
        argv[argc++] = coh_node_id;
    }
    if (cmdline_get_value("obmm_coh_test_node_count", coh_node_count,
                          sizeof(coh_node_count))) {
        argv[argc++] = "--node-count";
        argv[argc++] = coh_node_count;
    }
    if (cmdline_get_value("obmm_coh_test_token_value", coh_token_value,
                          sizeof(coh_token_value))) {
        argv[argc++] = "--token-value";
        argv[argc++] = coh_token_value;
    }
    if (cmdline_get_value("obmm_coh_test_generation", coh_generation,
                          sizeof(coh_generation))) {
        argv[argc++] = "--generation";
        argv[argc++] = coh_generation;
    }
    if (cmdline_has_option("obmm_coh_test_exporter=1")) {
        argv[argc++] = "--is-exporter";
    }
    if (cmdline_has_option("obmm_coh_test_verbose=1")) {
        argv[argc++] = "--verbose";
    }
    argv[argc] = NULL;

    pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[init] fork for obmm coh test failed: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) {
        execv("/bin/linqu_ub_obmm_coh_test", argv);
        fprintf(stderr, "[init] exec /bin/linqu_ub_obmm_coh_test failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    for (;;) {
        wait_ret = waitpid(pid, &status, WNOHANG);
        if (wait_ret == pid) {
            break;
        }
        if (wait_ret < 0) {
            fprintf(stderr, "[init] waitpid obmm coh test failed: %s\n", strerror(errno));
            return;
        }
        if (waited_ms >= 120000) {
            fprintf(stderr, "[init] ub obmm coh test app timeout, killing pid=%d\n",
                    pid);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            timed_out = true;
            break;
        }
        usleep(100000);
        waited_ms += 100;
    }

    if (!timed_out && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        fprintf(stderr, "[init] ub obmm coh test app pass\n");
        return;
    }

    if (timed_out) {
        fprintf(stderr, "[init] ub obmm coh test app fail timeout\n");
    } else if (WIFEXITED(status)) {
        fprintf(stderr, "[init] ub obmm coh test app fail exit=%d\n", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        fprintf(stderr, "[init] ub obmm coh test app fail signal=%d\n", WTERMSIG(status));
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

int main(int argc, char *argv[])
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
    if (should_run_ub_rpc()) {
        wait_for_ipourma_interface(30);
        run_ub_rpc_probe();
    }
    if (should_run_ub_tcp_each_server()) {
        wait_for_ipourma_interface(30);
        run_ub_tcp_each_server_probe();
    }
    if (should_run_ub_udma()) {
        wait_for_ipourma_interface(30);
        run_ub_udma_probe();
    }
    if (should_run_obmm_pool()) {
        wait_for_ipourma_interface(30);
        run_obmm_pool_probe();
    }
    if (should_run_obmm_queue()) {
        wait_for_ipourma_interface(30);
        run_obmm_queue_probe();
    }
    if (should_run_obmm_dataplane_microbench()) {
        wait_for_ipourma_interface(30);
        run_obmm_dataplane_microbench_probe();
    }
    if (should_run_obmm_import_stress()) {
        wait_for_ipourma_interface(30);
        run_obmm_import_stress_probe();
    }
    if (should_run_obmm_gsva()) {
        wait_for_ipourma_interface(30);
        run_obmm_gsva_probe();
    }
    if (should_run_gsva_query()) {
        wait_for_ipourma_interface(30);
        run_gsva_query_probe();
    }
    if (should_run_gsva_coh_test()) {
        wait_for_ipourma_interface(30);
        run_gsva_coh_test_probe();
    }
    if (should_run_gsva_lifecycle_test()) {
        wait_for_ipourma_interface(30);
        run_gsva_lifecycle_test_probe();
    }
    if (should_run_npu_gsva_test()) {
        wait_for_ipourma_interface(30);
        run_npu_gsva_test_probe();
    }
    if (should_run_npu_test()) {
        wait_for_ipourma_interface(30);
        run_npu_test_probe();
    }
    if (should_run_ssd_gsva_test()) {
        wait_for_ipourma_interface(30);
        run_ssd_gsva_test_probe();
    }
    if (should_run_ssd_test()) {
        wait_for_ipourma_interface(30);
        run_ssd_test_probe();
    }
    if (should_run_gva_direct()) {
        wait_for_ipourma_interface(30);
        run_gva_direct_probe();
    }
    if (should_run_obmm_coh_test()) {
        wait_for_ipourma_interface(30);
        run_obmm_coh_test_probe();
    }
    if (should_run_linqu_probe()) {
        run_probe();
    } else {
        fprintf(stderr, "[init] linqu_probe skipped by cmdline\n");
    }
    dump_ub_state();

    if (should_enter_app_boot_flow() && access("/bin/run_app", X_OK) == 0) {
        int i;
        char **new_argv = malloc(sizeof(char *) * (argc + 2));
        fprintf(stderr, "[init] switching to /bin/run_app app boot flow\n");
        new_argv[0] = "/bin/run_app";
        new_argv[1] = "--resume";
        for (i = 1; i < argc; i++) {
            new_argv[i + 1] = argv[i];
        }
        new_argv[argc + 1] = NULL;
        execv("/bin/run_app", new_argv);
        fprintf(stderr, "[init] exec /bin/run_app failed: %s\n", strerror(errno));
        free(new_argv);
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
