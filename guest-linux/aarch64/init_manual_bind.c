#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void ensure_dir(const char *path)
{
    if (mkdir(path, 0755) && errno != EEXIST)
        perror(path);
}

static void try_mount(const char *source, const char *target,
                       const char *type, unsigned long flags)
{
    if (mount(source, target, type, flags, NULL))
        fprintf(stderr, "mount %s on %s failed: %s\n",
                source, target, strerror(errno));
}

static void write_sysfs(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s for write failed: %s\n", path, strerror(errno));
        return;
    }
    if (write(fd, val, strlen(val)) < 0)
        fprintf(stderr, "write '%s' to %s failed: %s\n", val, path, strerror(errno));
    close(fd);
}

static void cat_file(const char *path)
{
    char buf[256];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s\n", path, strerror(errno));
        return;
    }
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("[TEST] %s: %s\n", path, buf);
    }
    close(fd);
}

static void try_insmod(const char *path)
{
    FILE *f;
    char cmd[256];
    int status;

    printf("[TEST] Loading module: %s\n", path);

    /* Check if module exists */
    if (access(path, F_OK)) {
        printf("[TEST] ERROR: Module not found: %s\n", path);
        return;
    }

    snprintf(cmd, sizeof(cmd), "/bin/insmod %s 2>&1", path);
    f = popen(cmd, "r");
    if (f) {
        char buf[256];
        int has_output = 0;
        while (fgets(buf, sizeof(buf), f)) {
            printf("[insmod] %s", buf);
            has_output = 1;
        }
        status = pclose(f);
        if (status == 0) {
            printf("[TEST] Module loaded successfully: %s\n", path);
        } else {
            printf("[TEST] ERROR: Failed to load %s (exit code %d)\n", path, status);
        }
    } else {
        printf("[TEST] ERROR: Failed to run insmod for %s\n", path);
    }
}

int main(void)
{
    puts("[TEST] Manual bind test for device 00001");

    ensure_dir("/proc");
    ensure_dir("/sys");
    ensure_dir("/dev");

    try_mount("none", "/proc", "proc", 0);
    try_mount("none", "/sys", "sysfs", 0);
    try_mount("none", "/dev", "devtmpfs", 0);

    sleep(1);

    puts("[TEST] Loading kernel modules...");
    try_insmod("/lib/modules/ubus.ko");
    try_insmod("/lib/modules/hisi_ubus.ko");
    try_insmod("/lib/modules/obmm.ko");
    try_insmod("/lib/modules/ub-sim-decoder.ko");
    try_insmod("/lib/modules/ubcore.ko");
    try_insmod("/lib/modules/ubase.ko");
    try_insmod("/lib/modules/ipourma.ko");
    try_insmod("/lib/modules/ummu-core.ko");
    try_insmod("/lib/modules/ummu.ko");
    try_insmod("/lib/modules/udma.ko");

    sleep(2);

    puts("[TEST] Checking if device 00001 exists");
    system("ls -l /sys/bus/ub/devices");

    if (access("/sys/bus/ub/devices/00001", F_OK)) {
        puts("[TEST] ERROR: Device 00001 does not exist!");
        puts("[TEST] Checking dmesg for enumeration...");
        system("dmesg | grep -Ei 'ubus|enum|probe' | tail -30");
        return 1;
    }

    puts("[TEST] Device 00001 exists!");
    puts("[TEST] Current match_driver value:");
    cat_file("/sys/bus/ub/devices/00001/match_driver");

    puts("[TEST] Setting match_driver to 1");
    write_sysfs("/sys/bus/ub/devices/00001/match_driver", "1");

    puts("[TEST] After setting, match_driver value:");
    cat_file("/sys/bus/ub/devices/00001/match_driver");

    puts("[TEST] Setting driver_override to ubase");
    write_sysfs("/sys/bus/ub/devices/00001/driver_override", "ubase");

    puts("[TEST] Triggering manual probe");
    write_sysfs("/sys/bus/ub/drivers_probe", "00001");

    sleep(2);

    puts("[TEST] Checking if driver is now bound");
    system("readlink /sys/bus/ub/devices/00001/driver || echo '[TEST] No driver bound'");

    puts("[TEST] Checking auxiliary devices");
    system("ls -l /sys/bus/auxiliary/devices");

    puts("[TEST] Checking network interfaces");
    system("ls /sys/class/net");

    puts("[TEST] Kernel log for ubase/auxiliary/udma/ipourma");
    system("dmesg | grep -Ei 'ubase|auxiliary|udma|ipourma' | tail -30");

    puts("[TEST] Test complete, sleeping for inspection");
    sleep(10);

    return 0;
}
