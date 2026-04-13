#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define DT_ROOT "/proc/device-tree"
#define FALLBACK_MMIO_BASE 0x0c000000ULL
#define UBC_RESOURCE_BASE_FALLBACK 0x18000000000ULL
#define LINQU_ENDPOINT1_OFFSET 0x1000ULL
#define UART0_MMIO_BASE    0x09000000ULL
#define PAGE_SIZE_BYTES 4096ULL

#define REG_VERSION    0x000
#define REG_CMDQ_BASE_LO 0x010
#define REG_CMDQ_BASE_HI 0x018
#define REG_CMDQ_SIZE  0x020
#define REG_CMDQ_HEAD  0x028
#define REG_CMDQ_TAIL  0x030
#define REG_CQ_BASE_LO 0x038
#define REG_CQ_BASE_HI 0x040
#define REG_CQ_SIZE    0x048
#define REG_CQ_HEAD    0x050
#define REG_CQ_TAIL    0x058
#define REG_STATUS     0x060
#define REG_DOORBELL   0x068
#define REG_LAST_ERROR 0x070
#define REG_IRQ_STATUS 0x078
#define REG_IRQ_ACK    0x080
#define REG_DEFAULT_SEGMENT 0x088

#define UART_REG_DR 0x000
#define UART_REG_FR 0x018
#define UART_REG_IBRD 0x024
#define UART_REG_FBRD 0x028
#define UART_REG_LCR_H 0x02c
#define UART_REG_CR 0x030

struct linqu_dt_info {
    bool found;
    char node_path[512];
    uint64_t base;
    uint64_t size;
    uint32_t irq_type;
    uint32_t irq_num;
    uint32_t irq_flags;
};

struct completion_preview {
    uint64_t op_id;
    uint8_t task_flag;
    uint8_t source;
    uint8_t status;
    uint64_t finished_at;
};

struct completion_counts {
    uint64_t shmem;
    uint64_t dfs;
    uint64_t db;
    uint64_t success;
    uint64_t retryable;
    uint64_t fatal;
};

static bool read_file_bytes(const char *path, uint8_t *buf, size_t len, size_t *out_len);
static bool cmdline_has_flag(const char *flag);
static int try_uio_irq_probe(volatile uint64_t *ep_regs);
static int try_guest_driver_irq_probe(volatile uint64_t *ep_regs);

static bool find_ubc_resource_base_from_sysfs(uint64_t *base_out)
{
    DIR *dir;
    struct dirent *ent;
    char resource_path[512];
    uint8_t line[256];
    size_t n = 0;

    dir = opendir("/sys/bus/platform/devices");
    if (!dir) {
        return false;
    }

    while ((ent = readdir(dir)) != NULL) {
        char *space = NULL;
        uint64_t start = 0;

        if (!strstr(ent->d_name, ".ubc")) {
            continue;
        }

        snprintf(resource_path, sizeof(resource_path),
                 "/sys/bus/platform/devices/%s/resource", ent->d_name);
        if (!read_file_bytes(resource_path, line, sizeof(line) - 1, &n) || n == 0) {
            continue;
        }

        line[n] = '\0';
        space = strchr((char *)line, ' ');
        if (space) {
            *space = '\0';
        }

        errno = 0;
        start = strtoull((char *)line, NULL, 16);
        if (errno == 0 && start != 0) {
            *base_out = start;
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static void print_hex64(const char *label, uint64_t value)
{
    printf("%s=0x%016" PRIx64 "\n", label, value);
}

static int expect_eq_u64(const char *label, uint64_t actual, uint64_t expected)
{
    if (actual != expected) {
        fprintf(stderr,
                "assertion-failed:%s actual=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
                label, actual, expected);
        return -1;
    }
    printf("assertion-ok:%s=0x%016" PRIx64 "\n", label, actual);
    return 0;
}

static void dump_text_file_matches(const char *path, const char *needle1, const char *needle2)
{
    FILE *fp = fopen(path, "r");
    char line[512];

    if (!fp) {
        printf("%s=open-failed:%s\n", path, strerror(errno));
        return;
    }

    printf("%s=begin\n", path);
    while (fgets(line, sizeof(line), fp) != NULL) {
        if ((needle1 && strstr(line, needle1)) || (needle2 && strstr(line, needle2))) {
            fputs(line, stdout);
        }
    }
    printf("%s=end\n", path);
    fclose(fp);
}

static void dump_binary_file_hex(const char *path)
{
    uint8_t buf[64];
    size_t i;
    size_t n = 0;

    if (!read_file_bytes(path, buf, sizeof(buf), &n)) {
        printf("%s=read-failed:%s\n", path, strerror(errno));
        return;
    }

    printf("%s=", path);
    for (i = 0; i < n; ++i) {
        printf("%02x", buf[i]);
        if (i + 1 != n) {
            putchar(':');
        }
    }
    putchar('\n');
}

static bool read_file_bytes(const char *path, uint8_t *buf, size_t len, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return false;
    }
    n = read(fd, buf, len);
    close(fd);
    if (n < 0) {
        return false;
    }
    *out_len = (size_t)n;
    return true;
}

static bool cmdline_has_flag(const char *flag)
{
    char buf[1024];
    size_t n = 0;

    if (!read_file_bytes("/proc/cmdline", (uint8_t *)buf, sizeof(buf) - 1, &n)) {
        return false;
    }
    buf[n] = '\0';
    return strstr(buf, flag) != NULL;
}

static int try_uio_irq_probe(volatile uint64_t *ep_regs)
{
    int fd;
    uint32_t irq_count = 0;
    ssize_t n;

    if (!cmdline_has_flag("linqu_probe_interrupt_uio=1")) {
        return 0;
    }

    puts("uio-irq-probe");
    fd = open("/dev/uio0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "uio.open.failed:%s\n", strerror(errno));
        return 1;
    }

    print_hex64("uio.irq_status.before_wait", ep_regs[REG_IRQ_STATUS / 8]);
    n = read(fd, &irq_count, sizeof(irq_count));
    if (n != (ssize_t)sizeof(irq_count)) {
        fprintf(stderr, "uio.read.failed:%s\n", strerror(errno));
        close(fd);
        return 1;
    }

    print_hex64("uio.irq_count", irq_count);
    print_hex64("uio.irq_status.after_wait", ep_regs[REG_IRQ_STATUS / 8]);

    irq_count = 1;
    n = write(fd, &irq_count, sizeof(irq_count));
    if (n != (ssize_t)sizeof(irq_count)) {
        fprintf(stderr, "uio.write.failed:%s\n", strerror(errno));
        close(fd);
        return 1;
    }
    print_hex64("uio.irq_status.after_reenable", ep_regs[REG_IRQ_STATUS / 8]);
    close(fd);
    return 0;
}

static int try_guest_driver_irq_probe(volatile uint64_t *ep_regs)
{
    struct {
        uint64_t irq_count;
        uint64_t irq_status;
    } snapshot = {0};
    int fd;
    struct pollfd pfd;
    ssize_t n;

    puts("guest-driver-irq-probe.check");
    if (!cmdline_has_flag("linqu_probe_guest_driver=1")) {
        puts("guest-driver-irq-probe.skip");
        return 0;
    }

    puts("guest-driver-irq-probe");
    fd = open("/dev/linqu-ub0", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "guest_driver.open.failed:%s\n", strerror(errno));
        return 1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    puts("guest-driver-irq-probe.poll");
    n = poll(&pfd, 1, 1000);
    if (n == 0) {
        print_hex64("guest_driver.poll.timeout.irq_status", ep_regs[REG_IRQ_STATUS / 8]);
        close(fd);
        return 1;
    }
    if (n < 0) {
        fprintf(stderr, "guest_driver.poll.failed:%s\n", strerror(errno));
        close(fd);
        return 1;
    }

    n = read(fd, &snapshot, sizeof(snapshot));
    puts("guest-driver-irq-probe.read");
    close(fd);
    if (n != (ssize_t)sizeof(snapshot)) {
        fprintf(stderr, "guest_driver.read.failed:%s\n", strerror(errno));
        return 1;
    }

    print_hex64("guest_driver.irq_count", snapshot.irq_count);
    print_hex64("guest_driver.irq_status", snapshot.irq_status);
    print_hex64("guest_driver.mmio_irq_status.after_wait", ep_regs[REG_IRQ_STATUS / 8]);

    if (snapshot.irq_count == 0 || snapshot.irq_status == 0) {
        return 1;
    }
    return 2;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static bool parse_reg_prop(const char *path, uint64_t *base, uint64_t *size)
{
    uint8_t buf[16];
    size_t n = 0;

    if (!read_file_bytes(path, buf, sizeof(buf), &n) || n < 8) {
        return false;
    }
    if (n >= 16) {
        *base = ((uint64_t)be32(&buf[0]) << 32) | be32(&buf[4]);
        *size = ((uint64_t)be32(&buf[8]) << 32) | be32(&buf[12]);
    } else {
        *base = be32(&buf[0]);
        *size = be32(&buf[4]);
    }
    return true;
}

static bool parse_interrupts_prop(const char *path, uint32_t *itype, uint32_t *inum, uint32_t *iflags)
{
    uint8_t buf[12];
    size_t n = 0;

    if (!read_file_bytes(path, buf, sizeof(buf), &n) || n < 12) {
        return false;
    }
    *itype = be32(&buf[0]);
    *inum = be32(&buf[4]);
    *iflags = be32(&buf[8]);
    return true;
}

static bool find_linqu_node_recursive(const char *dir_path, struct linqu_dt_info *info)
{
    DIR *dir = opendir(dir_path);
    struct dirent *de;

    if (!dir) {
        return false;
    }

    while ((de = readdir(dir)) != NULL) {
        char path[768];
        char compat_path[896];
        uint8_t compat[128];
        size_t compat_len = 0;
        struct stat st;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", dir_path, de->d_name);
        if (lstat(path, &st) != 0) {
            continue;
        }
        if (!S_ISDIR(st.st_mode)) {
            continue;
        }

        snprintf(compat_path, sizeof(compat_path), "%s/compatible", path);
        if (read_file_bytes(compat_path, compat, sizeof(compat), &compat_len)) {
            if (memmem(compat, compat_len, "linqu,ub", strlen("linqu,ub")) != NULL) {
                char reg_path[896];
                char irq_path[896];

                info->found = true;
                strncpy(info->node_path, path, sizeof(info->node_path) - 1);
                info->node_path[sizeof(info->node_path) - 1] = '\0';

                snprintf(reg_path, sizeof(reg_path), "%s/reg", path);
                parse_reg_prop(reg_path, &info->base, &info->size);

                snprintf(irq_path, sizeof(irq_path), "%s/interrupts", path);
                parse_interrupts_prop(irq_path, &info->irq_type, &info->irq_num, &info->irq_flags);

                closedir(dir);
                return true;
            }
        }

        if (find_linqu_node_recursive(path, info)) {
            closedir(dir);
            return true;
        }
    }

    closedir(dir);
    return false;
}

static int probe_mmio(uint64_t root_base)
{
    uint64_t ep_base = root_base + LINQU_ENDPOINT1_OFFSET;
    uint64_t root_page_base = root_base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t root_page_off = root_base - root_page_base;
    uint64_t ep_page_base = ep_base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t ep_page_off = ep_base - ep_page_base;
    int fd;
    void *root_map;
    void *ep_map;
    volatile uint8_t *root_mmio;
    volatile uint8_t *ep_mmio;
    volatile uint64_t *root_regs;
    volatile uint64_t *ep_regs;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open(/dev/mem)");
        return 1;
    }

    root_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)root_page_base);
    if (root_map == MAP_FAILED) {
        close(fd);
        perror("mmap(/dev/mem root)");
        return 1;
    }

    ep_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)ep_page_base);
    close(fd);
    if (ep_map == MAP_FAILED) {
        munmap(root_map, PAGE_SIZE_BYTES);
        perror("mmap(/dev/mem endpoint)");
        return 1;
    }

    root_mmio = (volatile uint8_t *)root_map + root_page_off;
    ep_mmio = (volatile uint8_t *)ep_map + ep_page_off;
    root_regs = (volatile uint64_t *)root_mmio;
    ep_regs = (volatile uint64_t *)ep_mmio;

    print_hex64("mmio.root_base", root_base);
    print_hex64("mmio.version", root_regs[REG_VERSION / 8]);
    print_hex64("mmio.endpoint1_base", ep_base);
    print_hex64("mmio.cmdq_size", ep_regs[REG_CMDQ_SIZE / 8]);
    print_hex64("mmio.cq_size", ep_regs[REG_CQ_SIZE / 8]);
    print_hex64("mmio.cmdq_head.before", ep_regs[REG_CMDQ_HEAD / 8]);
    print_hex64("mmio.cmdq_tail.before", ep_regs[REG_CMDQ_TAIL / 8]);
    print_hex64("mmio.cq_head.before", ep_regs[REG_CQ_HEAD / 8]);
    print_hex64("mmio.cq_tail.before", ep_regs[REG_CQ_TAIL / 8]);
    print_hex64("mmio.status.before", ep_regs[REG_STATUS / 8]);
    print_hex64("mmio.irq_status.before", ep_regs[REG_IRQ_STATUS / 8]);
    print_hex64("mmio.last_error.before", ep_regs[REG_LAST_ERROR / 8]);

    munmap(ep_map, PAGE_SIZE_BYTES);
    munmap(root_map, PAGE_SIZE_BYTES);
    return 0;
}

static int probe_uart_mmio(uint64_t base)
{
    uint64_t page_base = base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t page_off = base - page_base;
    int fd;
    void *map;
    volatile uint8_t *mmio;
    volatile uint64_t *regs;

    fd = open("/dev/mem", O_RDONLY | O_SYNC);
    if (fd < 0) {
        perror("open(/dev/mem uart)");
        return 1;
    }

    map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ, MAP_SHARED, fd, (off_t)page_base);
    close(fd);
    if (map == MAP_FAILED) {
        perror("mmap(/dev/mem uart)");
        return 1;
    }

    mmio = (volatile uint8_t *)map + page_off;
    regs = (volatile uint64_t *)mmio;

    puts("uart-mmio-probe");
    print_hex64("uart.base", base);
    print_hex64("uart.dr", regs[UART_REG_DR / 8]);
    print_hex64("uart.fr", regs[UART_REG_FR / 8]);
    print_hex64("uart.ibrd", regs[UART_REG_IBRD / 8]);
    print_hex64("uart.fbrd", regs[UART_REG_FBRD / 8]);
    print_hex64("uart.lcr_h", regs[UART_REG_LCR_H / 8]);
    print_hex64("uart.cr", regs[UART_REG_CR / 8]);

    munmap(map, PAGE_SIZE_BYTES);
    return 0;
}

static void write_u8_le(uint8_t *buf, size_t *off, uint8_t value)
{
    buf[*off] = value;
    *off += 1;
}

static void write_u32_le(uint8_t *buf, size_t *off, uint32_t value)
{
    memcpy(buf + *off, &value, sizeof(value));
    *off += sizeof(value);
}

static void write_u64_le(uint8_t *buf, size_t *off, uint64_t value)
{
    memcpy(buf + *off, &value, sizeof(value));
    *off += sizeof(value);
}

static int phys_for_virt(void *ptr, uint64_t *phys_out)
{
    uint64_t virt = (uint64_t)(uintptr_t)ptr;
    uint64_t page_index = virt / PAGE_SIZE_BYTES;
    uint64_t page_off = virt % PAGE_SIZE_BYTES;
    uint64_t entry = 0;
    int fd;
    ssize_t n;

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        perror("open(/proc/self/pagemap)");
        return -1;
    }

    n = pread(fd, &entry, sizeof(entry), (off_t)(page_index * sizeof(entry)));
    close(fd);
    if (n != (ssize_t)sizeof(entry)) {
        perror("pread(/proc/self/pagemap)");
        return -1;
    }
    if ((entry & (1ULL << 63)) == 0) {
        fprintf(stderr, "pagemap entry not present\n");
        return -1;
    }

    *phys_out = ((entry & ((1ULL << 55) - 1)) * PAGE_SIZE_BYTES) + page_off;
    return 0;
}

static void dump_slot_hex(const char *label, const uint8_t *buf, size_t len)
{
    size_t i;

    printf("%s=", label);
    for (i = 0; i < len; ++i) {
        printf("%02x", buf[i]);
        if (i + 1 != len) {
            putchar(':');
        }
    }
    putchar('\n');
}

static void build_dbput_descriptor(uint8_t *slot, size_t slot_bytes)
{
    static const char key[] = "guest-probe-dbput";
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 7);
    write_u8_le(slot, &off, 0);
    write_u8_le(slot, &off, (uint8_t)(sizeof(key) - 1));
    memcpy(slot + off, key, sizeof(key) - 1);
    off += sizeof(key) - 1;
    write_u64_le(slot, &off, 16);
}

static void build_dbget_descriptor(uint8_t *slot, size_t slot_bytes)
{
    static const char key[] = "guest-probe-dbput";
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 8);
    write_u8_le(slot, &off, 0);
    write_u8_le(slot, &off, (uint8_t)(sizeof(key) - 1));
    memcpy(slot + off, key, sizeof(key) - 1);
}

static void build_dfswrite_descriptor(uint8_t *slot, size_t slot_bytes)
{
    static const char path[] = "/guest/probe/dfs-path";
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 6);
    write_u8_le(slot, &off, 0);
    write_u8_le(slot, &off, (uint8_t)(sizeof(path) - 1));
    memcpy(slot + off, path, sizeof(path) - 1);
    off += sizeof(path) - 1;
    write_u64_le(slot, &off, 32);
}

static void build_dfsread_descriptor(uint8_t *slot, size_t slot_bytes)
{
    static const char path[] = "/guest/probe/dfs-path";
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 5);
    write_u8_le(slot, &off, 0);
    write_u8_le(slot, &off, (uint8_t)(sizeof(path) - 1));
    memcpy(slot + off, path, sizeof(path) - 1);
}

static void build_shmemput_descriptor(uint8_t *slot, size_t slot_bytes,
                                      uint32_t requester_entity,
                                      uint64_t segment,
                                      uint64_t bytes)
{
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 3);
    write_u8_le(slot, &off, 0);
    write_u32_le(slot, &off, requester_entity);
    write_u64_le(slot, &off, segment);
    write_u64_le(slot, &off, bytes);
}

static void build_shmemget_descriptor(uint8_t *slot, size_t slot_bytes,
                                      uint32_t requester_entity,
                                      uint64_t segment,
                                      uint64_t bytes)
{
    size_t off = 0;

    memset(slot, 0, slot_bytes);
    write_u8_le(slot, &off, 4);
    write_u8_le(slot, &off, 0);
    write_u32_le(slot, &off, requester_entity);
    write_u64_le(slot, &off, segment);
    write_u64_le(slot, &off, bytes);
}

static int decode_completion_preview(const uint8_t *slot, size_t len,
                                     struct completion_preview *preview)
{
    if (len < 19) {
        puts("completion.preview=slot-too-small");
        return -1;
    }

    memset(preview, 0, sizeof(*preview));
    memcpy(&preview->op_id, slot, sizeof(preview->op_id));
    preview->task_flag = slot[8];
    preview->source = slot[9];
    preview->status = slot[10];
    memcpy(&preview->finished_at, slot + 11, sizeof(preview->finished_at));

    print_hex64("completion.op_id", preview->op_id);
    print_hex64("completion.task_flag", preview->task_flag);
    print_hex64("completion.source", preview->source);
    print_hex64("completion.status", preview->status);
    print_hex64("completion.finished_at", preview->finished_at);
    return 0;
}

static void build_mixed_descriptor(uint8_t *slot, size_t slot_bytes, size_t kind_index,
                                   uint64_t default_segment)
{
    switch (kind_index % 6) {
    case 0:
        build_dbput_descriptor(slot, slot_bytes);
        break;
    case 1:
        build_dbget_descriptor(slot, slot_bytes);
        break;
    case 2:
        build_dfswrite_descriptor(slot, slot_bytes);
        break;
    case 3:
        build_dfsread_descriptor(slot, slot_bytes);
        break;
    case 4:
        build_shmemput_descriptor(slot, slot_bytes, 0, default_segment, 128);
        break;
    case 5:
    default:
        build_shmemget_descriptor(slot, slot_bytes, 0, default_segment, 128);
        break;
    }
}

static void build_invalid_descriptor(uint8_t *slot, size_t slot_bytes)
{
    memset(slot, 0, slot_bytes);
    slot[0] = 0xff;
}

static void count_completion(const struct completion_preview *preview,
                             struct completion_counts *counts)
{
    switch (preview->source) {
    case 3:
        counts->shmem += 1;
        break;
    case 4:
        counts->dfs += 1;
        break;
    case 5:
        counts->db += 1;
        break;
    default:
        break;
    }

    switch (preview->status) {
    case 1:
        counts->success += 1;
        break;
    case 2:
        counts->retryable += 1;
        break;
    case 3:
        counts->fatal += 1;
        break;
    default:
        break;
    }
}

static int probe_ring_path(uint64_t root_base)
{
    uint64_t ep_base = root_base + LINQU_ENDPOINT1_OFFSET;
    uint64_t ep_page_base = ep_base & ~(PAGE_SIZE_BYTES - 1);
    uint64_t ep_page_off = ep_base - ep_page_base;
    int fd;
    void *ep_map;
    volatile uint8_t *ep_mmio;
    volatile uint64_t *ep_regs;
    uint8_t *cmdq;
    uint8_t *cq;
    uint64_t cmdq_phys = 0;
    uint64_t cq_phys = 0;
    struct completion_preview completions[6];
    uint64_t default_segment;
    uint64_t cmdq_head_after_ring;
    uint64_t cq_tail_after_ring;
    uint64_t status_after_ring;
    uint64_t irq_status_after_ring;
    uint64_t last_error_after_ring;
    uint64_t cq_head_after_consume;
    uint64_t status_after_consume;
    uint64_t irq_status_after_consume;
    uint64_t irq_status_after_ack;
    uint64_t cq_head_after_partial;
    uint64_t status_after_partial;
    uint64_t irq_status_after_partial;
    struct completion_counts stress_counts = {0};
    uint64_t stress_cmdq_head_after_ring;
    uint64_t stress_cq_tail_after_ring;
    uint64_t stress_last_error_after_ring;
    uint64_t stress_irq_status_after_ring;
    uint64_t stress_cq_head_after_consume;
    uint64_t stress_irq_status_after_consume;
    uint64_t stress_irq_status_after_ack;
    uint64_t invalid_cmdq_head_after_ring;
    uint64_t invalid_cq_tail_after_ring;
    uint64_t invalid_last_error_after_ring;
    uint64_t invalid_irq_status_after_ring;
    uint64_t invalid_irq_status_after_ack;
    uint64_t cmdq_range_tail_before_write;
    uint64_t cmdq_range_tail_after_write;
    uint64_t cmdq_range_last_error_after_write;
    uint64_t cmdq_range_irq_status_after_write;
    uint64_t cmdq_range_irq_status_after_ack;
    uint64_t overflow_cmdq_head_after_round1;
    uint64_t overflow_cmdq_head_after_round2;
    uint64_t overflow_cmdq_head_after_round3;
    uint64_t overflow_cq_tail_after_round3;
    uint64_t overflow_last_error_after_round3;
    uint64_t overflow_irq_status_after_round3;
    uint64_t overflow_cq_head_after_drain;
    uint64_t overflow_irq_status_after_drain;
    uint64_t overflow_cq_tail_after_repoll;
    uint64_t overflow_irq_status_after_repoll;
    uint64_t overflow_cq_head_after_repoll_consume;
    uint64_t overflow_irq_status_after_final_ack;
    size_t stress_batch = 18;
    size_t stress_start = 6;
    size_t stress_cq_start = 6;
    size_t overflow_round1_start = 24;
    size_t overflow_round1_batch = 31;
    size_t overflow_round2_start = 55;
    size_t overflow_round2_batch = 31;
    size_t overflow_round3_start = 86;
    size_t overflow_round3_batch = 2;
    int rc = 0;
    size_t i;
    bool backend_overflow_mode = cmdline_has_flag("linqu_probe_backend_overflow=1");
    bool guest_driver_mode = false;

    fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open(/dev/mem ring)");
        return 1;
    }
    ep_map = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)ep_page_base);
    close(fd);
    if (ep_map == MAP_FAILED) {
        perror("mmap(/dev/mem ring)");
        return 1;
    }

    cmdq = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANON, -1, 0);
    cq = mmap(NULL, PAGE_SIZE_BYTES, PROT_READ | PROT_WRITE,
              MAP_PRIVATE | MAP_ANON, -1, 0);
    if (cmdq == MAP_FAILED || cq == MAP_FAILED) {
        perror("mmap(queue pages)");
        if (cmdq != MAP_FAILED) {
            munmap(cmdq, PAGE_SIZE_BYTES);
        }
        if (cq != MAP_FAILED) {
            munmap(cq, PAGE_SIZE_BYTES);
        }
        munmap(ep_map, PAGE_SIZE_BYTES);
        return 1;
    }

    memset(cmdq, 0, PAGE_SIZE_BYTES);
    memset(cq, 0, PAGE_SIZE_BYTES);
    build_dbput_descriptor(cmdq, 64);

    if (phys_for_virt(cmdq, &cmdq_phys) != 0 || phys_for_virt(cq, &cq_phys) != 0) {
        munmap(cmdq, PAGE_SIZE_BYTES);
        munmap(cq, PAGE_SIZE_BYTES);
        munmap(ep_map, PAGE_SIZE_BYTES);
        return 1;
    }

    ep_mmio = (volatile uint8_t *)ep_map + ep_page_off;
    ep_regs = (volatile uint64_t *)ep_mmio;

    puts("ring-probe");
    print_hex64("ring.cmdq_phys", cmdq_phys);
    print_hex64("ring.cq_phys", cq_phys);
    default_segment = ep_regs[REG_DEFAULT_SEGMENT / 8];
    print_hex64("ring.default_segment", default_segment);
    build_dbput_descriptor(cmdq + (0 * 64), 64);
    build_dbget_descriptor(cmdq + (1 * 64), 64);
    build_dfswrite_descriptor(cmdq + (2 * 64), 64);
    build_dfsread_descriptor(cmdq + (3 * 64), 64);
    build_shmemput_descriptor(cmdq + (4 * 64), 64, 0, default_segment, 128);
    build_shmemget_descriptor(cmdq + (5 * 64), 64, 0, default_segment, 128);

    if (backend_overflow_mode) {
        uint64_t backend_cmdq_head_after_ring;
        uint64_t backend_cq_tail_after_ring;
        uint64_t backend_last_error_after_ring;
        uint64_t backend_irq_status_after_ring;
        uint64_t backend_irq_status_after_ack;

        puts("ring-backend-overflow-probe");
        ep_regs[REG_CMDQ_BASE_LO / 8] = (uint32_t)cmdq_phys;
        ep_regs[REG_CMDQ_BASE_HI / 8] = cmdq_phys >> 32;
        ep_regs[REG_CQ_BASE_LO / 8] = (uint32_t)cq_phys;
        ep_regs[REG_CQ_BASE_HI / 8] = cq_phys >> 32;
        ep_regs[REG_CQ_HEAD / 8] = 0;
        ep_regs[REG_CMDQ_TAIL / 8] = 6;
        ep_regs[REG_DOORBELL / 8] = 6;

        backend_cmdq_head_after_ring = ep_regs[REG_CMDQ_HEAD / 8];
        backend_cq_tail_after_ring = ep_regs[REG_CQ_TAIL / 8];
        backend_last_error_after_ring = ep_regs[REG_LAST_ERROR / 8];
        backend_irq_status_after_ring = ep_regs[REG_IRQ_STATUS / 8];
        print_hex64("backend_overflow.cmdq_head.after_ring", backend_cmdq_head_after_ring);
        print_hex64("backend_overflow.cq_tail.after_ring", backend_cq_tail_after_ring);
        print_hex64("backend_overflow.last_error.after_ring", backend_last_error_after_ring);
        print_hex64("backend_overflow.irq_status.after_ring", backend_irq_status_after_ring);

        ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
        backend_irq_status_after_ack = ep_regs[REG_IRQ_STATUS / 8];
        print_hex64("backend_overflow.irq_status.after_ack", backend_irq_status_after_ack);

        rc |= expect_eq_u64("backend_overflow.cmdq_head.after_ring", backend_cmdq_head_after_ring, 0);
        rc |= expect_eq_u64("backend_overflow.cq_tail.after_ring", backend_cq_tail_after_ring, 0);
        rc |= expect_eq_u64("backend_overflow.last_error.after_ring", backend_last_error_after_ring, 5);
        rc |= expect_eq_u64("backend_overflow.irq_status.after_ring", backend_irq_status_after_ring, 2);
        rc |= expect_eq_u64("backend_overflow.irq_status.after_ack", backend_irq_status_after_ack, 0);
        goto out;
    }

    dump_slot_hex("ring.cmdq_slot0.before", cmdq + (0 * 64), 32);
    dump_slot_hex("ring.cmdq_slot1.before", cmdq + (1 * 64), 32);
    dump_slot_hex("ring.cmdq_slot2.before", cmdq + (2 * 64), 32);
    dump_slot_hex("ring.cmdq_slot3.before", cmdq + (3 * 64), 32);
    dump_slot_hex("ring.cmdq_slot4.before", cmdq + (4 * 64), 32);
    dump_slot_hex("ring.cmdq_slot5.before", cmdq + (5 * 64), 32);

    ep_regs[REG_CMDQ_BASE_LO / 8] = (uint32_t)cmdq_phys;
    ep_regs[REG_CMDQ_BASE_HI / 8] = cmdq_phys >> 32;
    ep_regs[REG_CQ_BASE_LO / 8] = (uint32_t)cq_phys;
    ep_regs[REG_CQ_BASE_HI / 8] = cq_phys >> 32;
    ep_regs[REG_CQ_HEAD / 8] = 0;
    ep_regs[REG_CMDQ_TAIL / 8] = 6;

    print_hex64("ring.cmdq_tail.programmed", ep_regs[REG_CMDQ_TAIL / 8]);
    print_hex64("ring.irq_status.before_doorbell", ep_regs[REG_IRQ_STATUS / 8]);
    print_hex64("ring.status.before_doorbell", ep_regs[REG_STATUS / 8]);
    ep_regs[REG_DOORBELL / 8] = 6;
    print_hex64("ring.doorbell.written", 6);
    print_hex64("ring.irq_status.after_doorbell", ep_regs[REG_IRQ_STATUS / 8]);
    print_hex64("ring.status.after_doorbell_immediate", ep_regs[REG_STATUS / 8]);
    {
        int guest_driver_probe = try_guest_driver_irq_probe(ep_regs);
        if (guest_driver_probe == 1) {
            rc = 1;
            goto out;
        }
        if (guest_driver_probe == 2) {
            guest_driver_mode = true;
        }
    }
    if (try_uio_irq_probe(ep_regs) != 0) {
        rc = 1;
        goto out;
    }

    cmdq_head_after_ring = ep_regs[REG_CMDQ_HEAD / 8];
    cq_tail_after_ring = ep_regs[REG_CQ_TAIL / 8];
    status_after_ring = ep_regs[REG_STATUS / 8];
    irq_status_after_ring = ep_regs[REG_IRQ_STATUS / 8];
    last_error_after_ring = ep_regs[REG_LAST_ERROR / 8];

    print_hex64("ring.cmdq_head.after_ring", cmdq_head_after_ring);
    print_hex64("ring.cq_tail.after_ring", cq_tail_after_ring);
    print_hex64("ring.status.after_ring", status_after_ring);
    print_hex64("ring.irq_status.after_ring", irq_status_after_ring);
    print_hex64("ring.last_error.after_ring", last_error_after_ring);

    dump_slot_hex("ring.cq_slot0.after", cq + (0 * 64), 32);
    dump_slot_hex("ring.cq_slot1.after", cq + (1 * 64), 32);
    dump_slot_hex("ring.cq_slot2.after", cq + (2 * 64), 32);
    dump_slot_hex("ring.cq_slot3.after", cq + (3 * 64), 32);
    dump_slot_hex("ring.cq_slot4.after", cq + (4 * 64), 32);
    dump_slot_hex("ring.cq_slot5.after", cq + (5 * 64), 32);
    for (i = 0; i < 6; ++i) {
        if (decode_completion_preview(cq + (i * 64), 64, &completions[i]) != 0) {
            rc = 1;
            goto out;
        }
    }

    ep_regs[REG_CQ_HEAD / 8] = 3;
    cq_head_after_partial = ep_regs[REG_CQ_HEAD / 8];
    status_after_partial = ep_regs[REG_STATUS / 8];
    irq_status_after_partial = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("ring.cq_head.after_partial", cq_head_after_partial);
    print_hex64("ring.status.after_partial", status_after_partial);
    print_hex64("ring.irq_status.after_partial", irq_status_after_partial);

    ep_regs[REG_CQ_HEAD / 8] = ep_regs[REG_CQ_TAIL / 8];
    cq_head_after_consume = ep_regs[REG_CQ_HEAD / 8];
    status_after_consume = ep_regs[REG_STATUS / 8];
    irq_status_after_consume = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("ring.cq_head.after_consume", cq_head_after_consume);
    print_hex64("ring.status.after_consume", status_after_consume);
    print_hex64("ring.irq_status.after_consume", irq_status_after_consume);

    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    irq_status_after_ack = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("ring.irq_status.after_ack", irq_status_after_ack);

    rc |= expect_eq_u64("ring.default_segment.nonzero", default_segment != 0, 1);
    rc |= expect_eq_u64("ring.cmdq_head.after_ring", cmdq_head_after_ring, 6);
    rc |= expect_eq_u64("ring.cq_tail.after_ring", cq_tail_after_ring, 6);
    rc |= expect_eq_u64("ring.last_error.after_ring", last_error_after_ring, 0);
    rc |= expect_eq_u64("ring.irq_status.after_ring", irq_status_after_ring,
                        guest_driver_mode ? 0 : 1);
    rc |= expect_eq_u64("ring.cq_head.after_partial", cq_head_after_partial, 3);
    rc |= expect_eq_u64("ring.irq_status.after_partial", irq_status_after_partial,
                        guest_driver_mode ? 0 : 1);
    rc |= expect_eq_u64("ring.cq_head.after_consume", cq_head_after_consume, 6);
    rc |= expect_eq_u64("ring.irq_status.after_consume", irq_status_after_consume, 0);
    rc |= expect_eq_u64("ring.irq_status.after_ack", irq_status_after_ack, 0);

    rc |= expect_eq_u64("completion[0].op_id", completions[0].op_id, 1);
    rc |= expect_eq_u64("completion[0].source", completions[0].source, 3);
    rc |= expect_eq_u64("completion[0].status", completions[0].status, 1);
    rc |= expect_eq_u64("completion[1].op_id", completions[1].op_id, 2);
    rc |= expect_eq_u64("completion[1].source", completions[1].source, 3);
    rc |= expect_eq_u64("completion[1].status", completions[1].status, 1);
    rc |= expect_eq_u64("completion[2].op_id", completions[2].op_id, 1);
    rc |= expect_eq_u64("completion[2].source", completions[2].source, 4);
    rc |= expect_eq_u64("completion[2].status", completions[2].status, 1);
    rc |= expect_eq_u64("completion[3].op_id", completions[3].op_id, 2);
    rc |= expect_eq_u64("completion[3].source", completions[3].source, 4);
    rc |= expect_eq_u64("completion[3].status", completions[3].status, 1);
    rc |= expect_eq_u64("completion[4].op_id", completions[4].op_id, 1);
    rc |= expect_eq_u64("completion[4].source", completions[4].source, 5);
    rc |= expect_eq_u64("completion[4].status", completions[4].status, 1);
    rc |= expect_eq_u64("completion[5].op_id", completions[5].op_id, 2);
    rc |= expect_eq_u64("completion[5].source", completions[5].source, 5);
    rc |= expect_eq_u64("completion[5].status", completions[5].status, 1);

    puts("ring-stress-probe");
    memset(cq, 0, PAGE_SIZE_BYTES);
    ep_regs[REG_CQ_HEAD / 8] = 0;
    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];

    for (i = 0; i < stress_batch; ++i) {
        build_mixed_descriptor(cmdq + ((stress_start + i) * 64), 64, i, default_segment);
    }
    ep_regs[REG_CMDQ_TAIL / 8] = stress_start + stress_batch;
    print_hex64("stress.cmdq_tail.programmed", ep_regs[REG_CMDQ_TAIL / 8]);
    ep_regs[REG_DOORBELL / 8] = stress_batch;

    stress_cmdq_head_after_ring = ep_regs[REG_CMDQ_HEAD / 8];
    stress_cq_tail_after_ring = ep_regs[REG_CQ_TAIL / 8];
    stress_last_error_after_ring = ep_regs[REG_LAST_ERROR / 8];
    stress_irq_status_after_ring = ep_regs[REG_IRQ_STATUS / 8];

    print_hex64("stress.cmdq_head.after_ring", stress_cmdq_head_after_ring);
    print_hex64("stress.cq_tail.after_ring", stress_cq_tail_after_ring);
    print_hex64("stress.last_error.after_ring", stress_last_error_after_ring);
    print_hex64("stress.irq_status.after_ring", stress_irq_status_after_ring);

    for (i = 0; i < stress_batch; ++i) {
        struct completion_preview preview;

        if (decode_completion_preview(cq + ((stress_cq_start + i) * 64), 64, &preview) != 0) {
            rc = 1;
            goto out;
        }
        count_completion(&preview, &stress_counts);
    }

    print_hex64("stress.count.shmem", stress_counts.shmem);
    print_hex64("stress.count.dfs", stress_counts.dfs);
    print_hex64("stress.count.db", stress_counts.db);
    print_hex64("stress.count.success", stress_counts.success);
    print_hex64("stress.count.retryable", stress_counts.retryable);
    print_hex64("stress.count.fatal", stress_counts.fatal);

    ep_regs[REG_CQ_HEAD / 8] = ep_regs[REG_CQ_TAIL / 8];
    stress_cq_head_after_consume = ep_regs[REG_CQ_HEAD / 8];
    stress_irq_status_after_consume = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("stress.cq_head.after_consume", stress_cq_head_after_consume);
    print_hex64("stress.irq_status.after_consume", stress_irq_status_after_consume);

    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    stress_irq_status_after_ack = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("stress.irq_status.after_ack", stress_irq_status_after_ack);

    rc |= expect_eq_u64("stress.cmdq_head.after_ring", stress_cmdq_head_after_ring, stress_start + stress_batch);
    rc |= expect_eq_u64("stress.cq_tail.after_ring", stress_cq_tail_after_ring, stress_cq_start + stress_batch);
    rc |= expect_eq_u64("stress.last_error.after_ring", stress_last_error_after_ring, 0);
    rc |= expect_eq_u64("stress.irq_status.after_ring", stress_irq_status_after_ring,
                        guest_driver_mode ? 0 : 1);
    rc |= expect_eq_u64("stress.count.shmem", stress_counts.shmem, 6);
    rc |= expect_eq_u64("stress.count.dfs", stress_counts.dfs, 6);
    rc |= expect_eq_u64("stress.count.db", stress_counts.db, 6);
    rc |= expect_eq_u64("stress.count.success", stress_counts.success, 18);
    rc |= expect_eq_u64("stress.count.retryable", stress_counts.retryable, 0);
    rc |= expect_eq_u64("stress.count.fatal", stress_counts.fatal, 0);
    rc |= expect_eq_u64("stress.cq_head.after_consume", stress_cq_head_after_consume, stress_cq_start + stress_batch);
    rc |= expect_eq_u64("stress.irq_status.after_consume", stress_irq_status_after_consume, 0);
    rc |= expect_eq_u64("stress.irq_status.after_ack", stress_irq_status_after_ack, 0);

    puts("ring-overflow-probe");
    for (i = 0; i < overflow_round1_batch; ++i) {
        build_mixed_descriptor(cmdq + (((overflow_round1_start + i) % 32) * 64), 64, i, default_segment);
    }
    ep_regs[REG_CMDQ_TAIL / 8] = 23;
    ep_regs[REG_DOORBELL / 8] = overflow_round1_batch;
    overflow_cmdq_head_after_round1 = ep_regs[REG_CMDQ_HEAD / 8];
    print_hex64("overflow.cmdq_head.after_round1", overflow_cmdq_head_after_round1);

    for (i = 0; i < overflow_round2_batch; ++i) {
        build_mixed_descriptor(cmdq + (((overflow_round2_start + i) % 32) * 64), 64, i, default_segment);
    }
    ep_regs[REG_CMDQ_TAIL / 8] = 22;
    ep_regs[REG_DOORBELL / 8] = overflow_round2_batch;
    overflow_cmdq_head_after_round2 = ep_regs[REG_CMDQ_HEAD / 8];
    print_hex64("overflow.cmdq_head.after_round2", overflow_cmdq_head_after_round2);

    for (i = 0; i < overflow_round3_batch; ++i) {
        build_mixed_descriptor(cmdq + (((overflow_round3_start + i) % 32) * 64), 64, i, default_segment);
    }
    ep_regs[REG_CMDQ_TAIL / 8] = 24;
    ep_regs[REG_DOORBELL / 8] = overflow_round3_batch;
    overflow_cmdq_head_after_round3 = ep_regs[REG_CMDQ_HEAD / 8];
    overflow_cq_tail_after_round3 = ep_regs[REG_CQ_TAIL / 8];
    overflow_last_error_after_round3 = ep_regs[REG_LAST_ERROR / 8];
    overflow_irq_status_after_round3 = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("overflow.cmdq_head.after_round3", overflow_cmdq_head_after_round3);
    print_hex64("overflow.cq_tail.after_round3", overflow_cq_tail_after_round3);
    print_hex64("overflow.last_error.after_round3", overflow_last_error_after_round3);
    print_hex64("overflow.irq_status.after_round3", overflow_irq_status_after_round3);

    ep_regs[REG_CQ_HEAD / 8] = ep_regs[REG_CQ_TAIL / 8];
    overflow_cq_head_after_drain = ep_regs[REG_CQ_HEAD / 8];
    overflow_irq_status_after_drain = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("overflow.cq_head.after_drain", overflow_cq_head_after_drain);
    print_hex64("overflow.irq_status.after_drain", overflow_irq_status_after_drain);

    ep_regs[REG_DOORBELL / 8] = 0;
    overflow_cq_tail_after_repoll = ep_regs[REG_CQ_TAIL / 8];
    overflow_irq_status_after_repoll = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("overflow.cq_tail.after_repoll", overflow_cq_tail_after_repoll);
    print_hex64("overflow.irq_status.after_repoll", overflow_irq_status_after_repoll);

    ep_regs[REG_CQ_HEAD / 8] = ep_regs[REG_CQ_TAIL / 8];
    overflow_cq_head_after_repoll_consume = ep_regs[REG_CQ_HEAD / 8];
    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    overflow_irq_status_after_final_ack = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("overflow.cq_head.after_repoll_consume", overflow_cq_head_after_repoll_consume);
    print_hex64("overflow.irq_status.after_final_ack", overflow_irq_status_after_final_ack);

    rc |= expect_eq_u64("overflow.cmdq_head.after_round1", overflow_cmdq_head_after_round1, 23);
    rc |= expect_eq_u64("overflow.cmdq_head.after_round2", overflow_cmdq_head_after_round2, 22);
    rc |= expect_eq_u64("overflow.cmdq_head.after_round3", overflow_cmdq_head_after_round3, 24);
    rc |= expect_eq_u64("overflow.cq_tail.after_round3", overflow_cq_tail_after_round3, 23);
    rc |= expect_eq_u64("overflow.last_error.after_round3", overflow_last_error_after_round3, 0);
    rc |= expect_eq_u64("overflow.irq_status.after_round3", overflow_irq_status_after_round3,
                        guest_driver_mode ? 0 : 5);
    rc |= expect_eq_u64("overflow.cq_head.after_drain", overflow_cq_head_after_drain, 23);
    rc |= expect_eq_u64("overflow.irq_status.after_drain", overflow_irq_status_after_drain,
                        guest_driver_mode ? 0 : 4);
    rc |= expect_eq_u64("overflow.cq_tail.after_repoll", overflow_cq_tail_after_repoll, 24);
    rc |= expect_eq_u64("overflow.irq_status.after_repoll", overflow_irq_status_after_repoll,
                        guest_driver_mode ? 0 : 5);
    rc |= expect_eq_u64("overflow.cq_head.after_repoll_consume", overflow_cq_head_after_repoll_consume, 24);
    rc |= expect_eq_u64("overflow.irq_status.after_final_ack", overflow_irq_status_after_final_ack, 0);

    puts("ring-invalid-probe");
    memset(cq, 0, PAGE_SIZE_BYTES);
    ep_regs[REG_CQ_HEAD / 8] = 0;
    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    build_invalid_descriptor(cmdq + ((stress_start + stress_batch) * 64), 64);
    ep_regs[REG_CMDQ_TAIL / 8] = stress_start + stress_batch + 1;
    print_hex64("invalid.cmdq_tail.programmed", ep_regs[REG_CMDQ_TAIL / 8]);
    ep_regs[REG_DOORBELL / 8] = 1;

    invalid_cmdq_head_after_ring = ep_regs[REG_CMDQ_HEAD / 8];
    invalid_cq_tail_after_ring = ep_regs[REG_CQ_TAIL / 8];
    invalid_last_error_after_ring = ep_regs[REG_LAST_ERROR / 8];
    invalid_irq_status_after_ring = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("invalid.cmdq_head.after_ring", invalid_cmdq_head_after_ring);
    print_hex64("invalid.cq_tail.after_ring", invalid_cq_tail_after_ring);
    print_hex64("invalid.last_error.after_ring", invalid_last_error_after_ring);
    print_hex64("invalid.irq_status.after_ring", invalid_irq_status_after_ring);

    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    invalid_irq_status_after_ack = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("invalid.irq_status.after_ack", invalid_irq_status_after_ack);

    rc |= expect_eq_u64("invalid.cmdq_head.after_ring", invalid_cmdq_head_after_ring, stress_start + stress_batch);
    rc |= expect_eq_u64("invalid.cq_tail.after_ring", invalid_cq_tail_after_ring, stress_cq_start + stress_batch);
    rc |= expect_eq_u64("invalid.last_error.after_ring", invalid_last_error_after_ring, 7);
    rc |= expect_eq_u64("invalid.irq_status.after_ring", invalid_irq_status_after_ring,
                        guest_driver_mode ? 0 : 2);
    rc |= expect_eq_u64("invalid.irq_status.after_ack", invalid_irq_status_after_ack, 0);

    puts("cmdq-range-probe");
    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    cmdq_range_tail_before_write = ep_regs[REG_CMDQ_TAIL / 8];
    ep_regs[REG_CMDQ_TAIL / 8] = ep_regs[REG_CMDQ_SIZE / 8];
    cmdq_range_tail_after_write = ep_regs[REG_CMDQ_TAIL / 8];
    cmdq_range_last_error_after_write = ep_regs[REG_LAST_ERROR / 8];
    cmdq_range_irq_status_after_write = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("cmdq_range.tail.before_write", cmdq_range_tail_before_write);
    print_hex64("cmdq_range.tail.after_write", cmdq_range_tail_after_write);
    print_hex64("cmdq_range.last_error.after_write", cmdq_range_last_error_after_write);
    print_hex64("cmdq_range.irq_status.after_write", cmdq_range_irq_status_after_write);

    ep_regs[REG_IRQ_ACK / 8] = ep_regs[REG_IRQ_STATUS / 8];
    cmdq_range_irq_status_after_ack = ep_regs[REG_IRQ_STATUS / 8];
    print_hex64("cmdq_range.irq_status.after_ack", cmdq_range_irq_status_after_ack);

    rc |= expect_eq_u64("cmdq_range.tail.unchanged", cmdq_range_tail_after_write, cmdq_range_tail_before_write);
    rc |= expect_eq_u64("cmdq_range.last_error.after_write", cmdq_range_last_error_after_write, 6);
    rc |= expect_eq_u64("cmdq_range.irq_status.after_write", cmdq_range_irq_status_after_write,
                        guest_driver_mode ? 0 : 2);
    rc |= expect_eq_u64("cmdq_range.irq_status.after_ack", cmdq_range_irq_status_after_ack, 0);

out:
    munmap(cmdq, PAGE_SIZE_BYTES);
    munmap(cq, PAGE_SIZE_BYTES);
    munmap(ep_map, PAGE_SIZE_BYTES);
    return rc ? 1 : 0;
}

int main(void)
{
    struct linqu_dt_info info = {0};
    uint64_t base = UBC_RESOURCE_BASE_FALLBACK;

    puts("linqu-ub linux probe");
    if (find_linqu_node_recursive(DT_ROOT, &info)) {
        printf("dt.node=%s\n", info.node_path);
        print_hex64("dt.base", info.base);
        print_hex64("dt.size", info.size);
        printf("dt.irq=<%u %u %u>\n", info.irq_type, info.irq_num, info.irq_flags);
        {
            char reg_path[896];
            char ranges_path[896];

            snprintf(reg_path, sizeof(reg_path), "%s/reg", info.node_path);
            dump_binary_file_hex(reg_path);

            snprintf(ranges_path, sizeof(ranges_path), "%s/../ranges", info.node_path);
            dump_binary_file_hex(ranges_path);
        }
        if (info.base != 0) {
            base = info.base;
        }
    } else {
        puts("dt.node=not-found");
        if (find_ubc_resource_base_from_sysfs(&base)) {
            print_hex64("sysfs.ubc_base", base);
        } else {
            base = UBC_RESOURCE_BASE_FALLBACK;
            print_hex64("dt.base.fallback", FALLBACK_MMIO_BASE);
            print_hex64("sysfs.ubc_base.fallback", base);
        }
    }

    probe_uart_mmio(UART0_MMIO_BASE);
    dump_text_file_matches("/proc/iomem", "c000000", "linqu");
    probe_mmio(base);
    return probe_ring_path(base);
}
