/* SPDX-License-Identifier: GPL-2.0 */
/*
 * OBMM common helper functions -- shared across apps and tests.
 *
 * This header provides OBMM export/import, memory mapping, network setup,
 * and sysfs parsing utilities.  All functions are static to allow
 * single-file compilation (matching the existing project convention).
 *
 * Functions excluded from this header (remain in app-specific helpers):
 *   - obmm_resolve_nodes, obmm_parse_ip_list  (app-specific env/cmdline)
 *   - obmm_parse_export_size                   (app-specific env var)
 *   - obmm_create_udp, obmm_send_udp, obmm_recv_udp, obmm_init_pool_msg
 *                                               (UDP transport)
 *   - obmm_helpers_pool_msg, OBMM_MSG_HELLO, OBMM_MSG_READY
 *                                               (app-specific messages)
 *   - OBMM_POOL_HELPERS_PORT                   (app-specific constant)
 */

#ifndef OBMM_COMMON_H
#define OBMM_COMMON_H

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
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../../kernel_ub/include/uapi/ub/obmm.h"
#include "../../kernel_ub/include/uapi/ub/gsva.h"

#include <libobmm.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

#define OBMM_POOL_HELPERS_WAIT_IFACE_MS 90000
#define OBMM_POOL_HELPERS_IMPORT_ALIGN  (2UL * 1024UL * 1024UL)
#define OBMM_POOL_HELPERS_MAX_NODES     8
#define OBMM_POOL_HELPERS_MAX_WINDOWS   16
#define OBMM_MAYBE_UNUSED               __attribute__((unused))

enum obmm_import_cache_mode {
    OBMM_IMPORT_CACHE_AUTO = 0,
    OBMM_IMPORT_CACHE_NC   = 1,
    OBMM_IMPORT_CACHE_CC   = 2,
};
#define OBMM_POOL_HELPERS_MAGIC         0x4f424d50U
#define OBMM_POOL_HELPERS_VERSION       1U
#ifndef OBMM_EXPORT_FLAG_GSVA_FIXED_UBA
#define OBMM_EXPORT_FLAG_GSVA_FIXED_UBA 0x4UL
#endif
#ifndef OBMM_MMAP_FLAG_GSVA
#define OBMM_MMAP_FLAG_GSVA (1UL << 62)
#endif
#ifndef MAP_GSVA
#define MAP_GSVA 0x200000
#endif
#define OBMM_SIM_DEC_PRIV_MAGIC        0x53444950U
#define OBMM_SIM_DEC_PRIV_VER_1        1
#define OBMM_SIM_DEC_PRIV_VER_2        2
#define OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM 1
#define OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER 2
#define OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA 1
#define OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY 2
#define OBMM_SIM_DEC_CACHE_POLICY_NC 0
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_THROUGH 1
#define OBMM_SIM_DEC_CACHE_POLICY_READ_CACHE 2
#define OBMM_SIM_DEC_CACHE_POLICY_WRITE_BACK 3
#define OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI 4

#define OBMM_SIM_DEC_ACCESS_READ_ONLY (1U << 0)
#define OBMM_SIM_DEC_ACCESS_EXPLICIT_SYNC (1U << 1)
#define OBMM_SIM_DEC_ACCESS_FAULT_UPI_MISMATCH (1U << 31)

/* ------------------------------------------------------------------ */
/* Shared types                                                        */
/* ------------------------------------------------------------------ */

struct obmm_helpers_meta {
    uint64_t export_mem_id;
    uint64_t remote_uba;
    uint64_t size;
    uint32_t token_id;
    uint32_t export_cna;
};

struct obmm_helpers_region {
    int fd;
    void *addr;
    size_t len;
    uint64_t mem_id;
};

struct obmm_sim_dec_import_priv_v2 {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t remote_uba;
    uint32_t token_value;
    uint32_t flags;
    uint32_t map_source;
    uint32_t address_profile;
    uint32_t cache_policy;
    uint32_t vmid;
    uint32_t asid;
    uint64_t local_va;
    uint64_t home_va;
    uint64_t pte_offset;
    uint32_t tid;
    uint32_t p_tag;
    uint32_t access_flags;
    uint64_t gva_id;
    uint64_t segment_id;
    uint64_t epoch;
};

struct obmm_sim_dec_import_priv_v1 {
    uint32_t magic;
    uint16_t version;
    uint16_t len;
    uint64_t remote_uba;
    uint32_t token_value;
    uint32_t flags;
};

struct obmm_helpers_window {
    uint64_t base_pa;
    uint64_t size_bytes;
    uint64_t decode;
    unsigned int mar;
    bool is_cacheable;
};

/* ------------------------------------------------------------------ */
/* Alignment helper                                                    */
/* ------------------------------------------------------------------ */

static inline uint64_t obmm_align_up_u64(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

/* ------------------------------------------------------------------ */
/* Time and file utilities                                             */
/* ------------------------------------------------------------------ */

static long obmm_now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static bool obmm_read_file(const char *path, char *buf, size_t len)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0)
        return false;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';
    return true;
}

static bool OBMM_MAYBE_UNUSED obmm_parse_hex_u64(const char *path, uint64_t *value_out)
{
    char buf[128];
    char *end = NULL;
    unsigned long long value;
    if (!obmm_read_file(path, buf, sizeof(buf)))
        return false;
    errno = 0;
    value = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf)
        return false;
    *value_out = (uint64_t)value;
    return true;
}

/* ------------------------------------------------------------------ */
/* Command line and environment                                        */
/* ------------------------------------------------------------------ */

static bool obmm_cmdline_get(const char *key, char *out, size_t out_len)
{
    char buf[4096];
    char *saveptr = NULL;
    char *tok;
    size_t key_len;
    if (!obmm_read_file("/proc/cmdline", buf, sizeof(buf)))
        return false;
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

static bool OBMM_MAYBE_UNUSED obmm_env_or_cmdline(const char *env_key,
                                                  const char *cmd_key,
                                                  char *out,
                                                  size_t out_len)
{
    const char *env = getenv(env_key);
    if (env && env[0] != '\0') {
        snprintf(out, out_len, "%s", env);
        return true;
    }
    return obmm_cmdline_get(cmd_key, out, out_len);
}

/* ------------------------------------------------------------------ */
/* Network helpers                                                     */
/* ------------------------------------------------------------------ */

static bool obmm_find_iface(char *name, size_t name_len)
{
    FILE *fp = fopen("/proc/net/dev", "r");
    char line[512];
    if (!fp)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon = strchr(line, ':');
        char *left;
        size_t n;
        if (!colon)
            continue;
        *colon = '\0';
        left = line;
        while (*left == ' ' || *left == '\t')
            left++;
        if (strncmp(left, "ipourma", strlen("ipourma")) != 0)
            continue;
        n = strcspn(left, " \t\r\n");
        if (n >= name_len)
            n = name_len - 1;
        memcpy(name, left, n);
        name[n] = '\0';
        fclose(fp);
        return true;
    }
    fclose(fp);
    return false;
}

static bool obmm_iface_is_up(const char *ifname)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        close(fd);
        return false;
    }
    close(fd);
    return (ifr.ifr_flags & IFF_UP) != 0;
}

static bool OBMM_MAYBE_UNUSED obmm_wait_iface(char *ifname,
                                              size_t ifname_len,
                                              unsigned int *ifindex)
{
    long deadline = obmm_now_ms() + OBMM_POOL_HELPERS_WAIT_IFACE_MS;
    while (obmm_now_ms() < deadline) {
        if (obmm_find_iface(ifname, ifname_len)) {
            *ifindex = if_nametoindex(ifname);
            if (*ifindex > 0 && obmm_iface_is_up(ifname))
                return true;
        }
        usleep(200000);
    }
    return false;
}

static bool OBMM_MAYBE_UNUSED obmm_set_ipv4(const char *ifname, const char *addr_str)
{
    struct ifreq ifr;
    struct sockaddr_in *sin;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", ifname);
    sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    if (inet_pton(AF_INET, addr_str, &sin->sin_addr) != 1) {
        close(fd);
        return false;
    }
    if (ioctl(fd, SIOCSIFADDR, &ifr) != 0) {
        close(fd);
        return false;
    }
    memset(&ifr.ifr_netmask, 0, sizeof(ifr.ifr_netmask));
    sin = (struct sockaddr_in *)&ifr.ifr_netmask;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) != 0) {
        close(fd);
        return false;
    }
    close(fd);
    return true;
}

static bool OBMM_MAYBE_UNUSED obmm_get_local_ipv4(const char *ifname, struct in_addr *addr)
{
    struct ifreq ifr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return false;
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

static void OBMM_MAYBE_UNUSED obmm_install_arp(const char *ifname,
                                               const struct in_addr *peer_addr)
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
    if (fd < 0)
        return;
    (void)ioctl(fd, SIOCSARP, &req);
    close(fd);
}

/* ------------------------------------------------------------------ */
/* OBMM device operations                                              */
/* ------------------------------------------------------------------ */

static int OBMM_MAYBE_UNUSED obmm_open_device(void)
{
    return open("/dev/obmm", O_RDWR);
}

static int obmm_open_shmdev(uint64_t mem_id, bool map_osync)
{
    char path[128];
    snprintf(path, sizeof(path), "/dev/obmm_shmdev%" PRIu64, mem_id);
    return open(path, O_RDWR | (map_osync ? O_SYNC : 0));
}

static int obmm_map_region_at_offset(uint64_t mem_id, void *addr, size_t len,
                                     bool map_osync, uint64_t mmap_offset,
                                     struct obmm_helpers_region *region);

static int obmm_map_region_at_offset_flags(uint64_t mem_id, void *addr,
                                           size_t len, bool map_osync,
                                           uint64_t mmap_offset,
                                           int extra_mmap_flags,
                                           struct obmm_helpers_region *region)
{
    int flags = MAP_SHARED | extra_mmap_flags;
    void *mapped;
    memset(region, 0, sizeof(*region));
    region->fd = -1;
    region->mem_id = mem_id;
    region->len = len;
    region->fd = obmm_open_shmdev(mem_id, map_osync);
    if (region->fd < 0) {
        fprintf(stderr, "[obmm] open shmdev%" PRIu64 " failed: %s\n",
                mem_id, strerror(errno));
        return -1;
    }
    if (addr) {
        flags |= MAP_FIXED_NOREPLACE;
    }
    mapped = mmap(addr, len, PROT_READ | PROT_WRITE, flags, region->fd,
                  (off_t)mmap_offset);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "[obmm] mmap shmdev%" PRIu64 " at %p failed: %s\n",
                mem_id, addr, strerror(errno));
        close(region->fd);
        region->fd = -1;
        region->addr = NULL;
        return -1;
    }
    region->addr = mapped;
    return 0;
}

static int obmm_map_region_at_offset(uint64_t mem_id, void *addr, size_t len,
                                     bool map_osync, uint64_t mmap_offset,
                                     struct obmm_helpers_region *region)
{
    return obmm_map_region_at_offset_flags(mem_id, addr, len, map_osync,
                                           mmap_offset, 0, region);
}

static int obmm_map_region_at(uint64_t mem_id, void *addr, size_t len, bool map_osync,
                             struct obmm_helpers_region *region)
{
    return obmm_map_region_at_offset(mem_id, addr, len, map_osync, 0, region);
}

static int OBMM_MAYBE_UNUSED obmm_map_gsva_region_at(
    uint64_t mem_id, void *addr, size_t len, bool map_osync,
    struct obmm_helpers_region *region)
{
    return obmm_map_region_at_offset_flags(mem_id, addr, len, map_osync, 0,
                                           MAP_GSVA, region);
}

static int OBMM_MAYBE_UNUSED obmm_map_region(uint64_t mem_id, size_t len, bool map_osync,
                          struct obmm_helpers_region *region)
{
    return obmm_map_region_at(mem_id, NULL, len, map_osync, region);
}

static void OBMM_MAYBE_UNUSED obmm_unmap_region(struct obmm_helpers_region *region)
{
    if (region->addr && region->addr != MAP_FAILED) {
        munmap(region->addr, region->len);
        region->addr = NULL;
    }
    if (region->fd >= 0) {
        close(region->fd);
        region->fd = -1;
    }
}

static int OBMM_MAYBE_UNUSED obmm_do_export(int obmm_fd, struct obmm_helpers_meta *meta,
                          uint64_t export_size)
{
    size_t length[OBMM_MAX_LOCAL_NUMA_NODES] = {0};
    struct obmm_mem_desc desc;
    mem_id id;

    (void)obmm_fd;
    memset(&desc, 0, sizeof(desc));
    length[0] = (size_t)export_size;
    id = obmm_export(length, OBMM_EXPORT_FLAG_ALLOW_MMAP, &desc);
    if (id == OBMM_INVALID_MEMID)
        return -1;
    meta->export_mem_id = (uint64_t)id;
    meta->remote_uba = desc.addr;
    meta->size = export_size;
    meta->token_id = desc.tokenid;
    return 0;
}

static int OBMM_MAYBE_UNUSED obmm_do_export_fixed_uba(
    int obmm_fd, struct obmm_helpers_meta *meta, uint64_t export_size,
    uint64_t requested_uba)
{
    struct obmm_cmd_export cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.length = 1;
    cmd.size[0] = export_size;
    cmd.flags = OBMM_EXPORT_FLAG_ALLOW_MMAP | OBMM_EXPORT_FLAG_GSVA_FIXED_UBA;
    cmd.uba = requested_uba;
    cmd.pxm_numa = 0;
    if (ioctl(obmm_fd, OBMM_CMD_EXPORT, &cmd) != 0) {
        fprintf(stderr, "[obmm] fixed-uba export failed: requested_uba=%#"
                PRIx64 " errno=%d\n", requested_uba, errno);
        return -1;
    }
    meta->export_mem_id = cmd.mem_id;
    meta->remote_uba = cmd.uba;
    meta->size = export_size;
    meta->token_id = cmd.tokenid;
    return 0;
}

static int OBMM_MAYBE_UNUSED obmm_do_unexport(int obmm_fd, uint64_t mem_id)
{
    (void)obmm_fd;
    return obmm_unexport(mem_id, 0);
}

static int OBMM_MAYBE_UNUSED obmm_do_import(int obmm_fd, const struct obmm_helpers_meta *meta,
                          uint32_t local_cna, uint64_t local_pa,
                          uint32_t token_value, uint64_t *import_mem_id)
{
    struct obmm_sim_dec_import_priv_v1 priv;
    struct obmm_mem_desc *desc;
    int numa = 0;
    mem_id id;

    (void)obmm_fd;
    memset(&priv, 0, sizeof(priv));
    priv.magic = OBMM_SIM_DEC_PRIV_MAGIC;
    priv.version = OBMM_SIM_DEC_PRIV_VER_1;
    priv.len = sizeof(priv);
    priv.remote_uba = meta->remote_uba;
    priv.token_value = token_value;

    desc = calloc(1, sizeof(*desc) + sizeof(priv));
    if (!desc)
        return -1;
    desc->addr = local_pa;
    desc->length = meta->size;
    desc->tokenid = meta->token_id;
    desc->scna = local_cna;
    desc->dcna = meta->export_cna;
    desc->priv_len = sizeof(priv);
    memcpy(desc->priv, &priv, sizeof(priv));

    id = obmm_import(desc, OBMM_IMPORT_FLAG_ALLOW_MMAP, 0, &numa);
    free(desc);
    if (id == OBMM_INVALID_MEMID)
        return -1;
    *import_mem_id = (uint64_t)id;
    return 0;
}

static int obmm_do_import_v2_epoch(int obmm_fd,
                            const struct obmm_helpers_meta *meta,
                            uint32_t local_cna, uint64_t local_pa,
                            uint32_t token_value, uint32_t map_source,
                            uint32_t address_profile, uint32_t cache_policy,
                            uint32_t vmid, uint32_t asid, uint32_t tid,
                            uint32_t p_tag, uint32_t access_flags,
                            uint64_t gva_id, uint64_t epoch,
                            uint64_t local_va, uint64_t home_va,
                            uint64_t pte_offset, uint64_t *import_mem_id)
{
    struct obmm_sim_dec_import_priv_v2 priv = {0};
    struct obmm_mem_desc *desc;
    int numa = 0;
    mem_id id;

    priv.magic = OBMM_SIM_DEC_PRIV_MAGIC;
    priv.version = OBMM_SIM_DEC_PRIV_VER_2;
    priv.len = sizeof(priv);
    priv.remote_uba = meta->remote_uba;
    priv.token_value = token_value;
    priv.map_source = map_source;
    priv.address_profile = address_profile;
    priv.cache_policy = cache_policy;
    priv.vmid = vmid;
    priv.asid = asid;
    priv.local_va = local_va;
    priv.home_va = home_va;
    priv.pte_offset = pte_offset;
    priv.tid = tid;
    priv.p_tag = p_tag;
    priv.access_flags = access_flags;
    priv.gva_id = gva_id;
    priv.segment_id = gva_id;
    priv.epoch = epoch;

    (void)obmm_fd;
    desc = calloc(1, sizeof(*desc) + sizeof(priv));
    if (!desc)
        return -1;
    desc->addr = local_pa;
    desc->length = meta->size;
    desc->tokenid = meta->token_id;
    desc->scna = local_cna;
    desc->dcna = meta->export_cna;
    desc->priv_len = sizeof(priv);
    memcpy(desc->priv, &priv, sizeof(priv));

    id = obmm_import(desc, OBMM_IMPORT_FLAG_ALLOW_MMAP, 0, &numa);
    free(desc);
    if (id == OBMM_INVALID_MEMID)
        return -1;
    *import_mem_id = (uint64_t)id;
    return 0;
}

static int OBMM_MAYBE_UNUSED obmm_do_import_v2(int obmm_fd, const struct obmm_helpers_meta *meta,
                            uint32_t local_cna, uint64_t local_pa,
                            uint32_t token_value, uint32_t map_source,
                            uint32_t address_profile, uint32_t cache_policy,
                            uint32_t vmid, uint32_t asid, uint32_t tid,
                            uint32_t p_tag, uint32_t access_flags,
                            uint64_t gva_id, uint64_t local_va,
                            uint64_t home_va, uint64_t pte_offset,
                            uint64_t *import_mem_id)
{
    return obmm_do_import_v2_epoch(obmm_fd, meta, local_cna, local_pa,
                                   token_value, map_source, address_profile,
                                   cache_policy, vmid, asid, tid, p_tag,
                                   access_flags, gva_id, 1, local_va, home_va,
                                   pte_offset, import_mem_id);
}

static int OBMM_MAYBE_UNUSED obmm_do_import_gsva_desc_v1(
                            int obmm_fd,
                            const struct obmm_gsva_segment_desc_v1 *desc,
                            uint32_t local_cna, uint64_t local_pa,
                            uint64_t local_va, uint64_t *import_mem_id)
{
    struct obmm_helpers_meta meta = {0};

    if (!desc || desc->version != OBMM_GSVA_ABI_VERSION ||
        !(desc->flags & OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY) ||
        local_va != desc->home_va) {
        errno = EINVAL;
        return -1;
    }

    meta.export_mem_id = desc->segment_id;
    meta.remote_uba = desc->home_va;
    meta.size = desc->size;
    meta.token_id = desc->token_id;
    meta.export_cna = desc->home_cna;

    return obmm_do_import_v2_epoch(obmm_fd, &meta, local_cna, local_pa,
                                   desc->token_value,
                                   OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
                                   OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
                                   desc->cache_policy,
                                   0, 0, 0, desc->p_tag,
                                   desc->access_flags,
                                   desc->segment_id, desc->epoch,
                                   desc->home_va, desc->home_va, 0,
                                   import_mem_id);
}

static int OBMM_MAYBE_UNUSED obmm_bootstrap_publish(int obmm_fd, int local_idx, int node_count,
                                  uint64_t generation,
                                  const struct obmm_helpers_meta *meta)
{
    struct obmm_cmd_bootstrap_publish cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.record.export_mem_id = meta->export_mem_id;
    cmd.record.remote_uba = meta->remote_uba;
    cmd.record.size = meta->size;
    cmd.record.generation = generation;
    cmd.record.node_id = (uint32_t)local_idx;
    cmd.record.node_count = (uint32_t)node_count;
    cmd.record.export_cna = meta->export_cna;
    cmd.record.token_id = meta->token_id;
    if (ioctl(obmm_fd, OBMM_CMD_BOOTSTRAP_PUBLISH, &cmd) != 0)
        return -1;
    return 0;
}

static int OBMM_MAYBE_UNUSED obmm_bootstrap_lookup(int obmm_fd, uint32_t local_cna,
                                 int node_count, uint64_t generation,
                                 struct obmm_helpers_meta metas[
                                     OBMM_POOL_HELPERS_MAX_NODES],
                                 bool got[OBMM_POOL_HELPERS_MAX_NODES])
{
    long deadline = obmm_now_ms() + OBMM_POOL_HELPERS_WAIT_IFACE_MS;
    int i;

    while (obmm_now_ms() < deadline) {
        struct obmm_cmd_bootstrap_lookup cmd;
        bool all = true;

        memset(&cmd, 0, sizeof(cmd));
        cmd.generation = generation;
        cmd.node_count = (uint32_t)node_count;
        cmd.local_cna = local_cna;
        if (ioctl(obmm_fd, OBMM_CMD_BOOTSTRAP_LOOKUP, &cmd) != 0)
            return -1;

        for (i = 0; i < (int)cmd.count; i++) {
            struct obmm_bootstrap_record *record = &cmd.records[i];
            if (record->node_id >= (uint32_t)node_count)
                continue;
            metas[record->node_id].export_mem_id = record->export_mem_id;
            metas[record->node_id].remote_uba = record->remote_uba;
            metas[record->node_id].size = record->size;
            metas[record->node_id].token_id = record->token_id;
            metas[record->node_id].export_cna = record->export_cna;
            got[record->node_id] = true;
        }

        for (i = 0; i < node_count; i++) {
            if (!got[i]) {
                all = false;
                break;
            }
        }
        if (all)
            return 0;
        usleep(100000);
    }
    errno = ETIMEDOUT;
    return -1;
}

static int OBMM_MAYBE_UNUSED obmm_do_unimport(int obmm_fd, uint64_t mem_id)
{
    (void)obmm_fd;
    return obmm_unimport(mem_id, 0);
}

/* ------------------------------------------------------------------ */
/* Memory window parsing                                               */
/* ------------------------------------------------------------------ */

static bool obmm_parse_windows(struct obmm_helpers_window windows[
                                   OBMM_POOL_HELPERS_MAX_WINDOWS],
                               int *count_out,
                               enum obmm_import_cache_mode cache_mode)
{
    FILE *fp;
    char line[256];
    int count = 0;
    fp = fopen("/sys/bus/ub/devices/00001/mem_windows", "r");
    if (!fp)
        return false;
    while (fgets(line, sizeof(line), fp) != NULL) {
        unsigned long long mar = 0, decode = 0;
        unsigned long long cc_base_mb = 0, cc_size_mb = 0;
        unsigned long long nc_base_mb = 0, nc_size_mb = 0;
        int matched;
        matched = sscanf(line,
            "mar%llu decode=%llx cc_base_mb=%llx cc_size_mb=%llx "
            "nc_base_mb=%llx nc_size_mb=%llx",
            &mar, &decode, &cc_base_mb, &cc_size_mb,
            &nc_base_mb, &nc_size_mb);
        if (matched != 6)
            continue;
        if (cache_mode == OBMM_IMPORT_CACHE_CC) {
            if (cc_size_mb == 0)
                continue;
            if (count >= OBMM_POOL_HELPERS_MAX_WINDOWS)
                break;
            windows[count].mar = (unsigned int)mar;
            windows[count].decode = (uint64_t)decode;
            windows[count].base_pa = ((uint64_t)cc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)cc_size_mb) << 20;
            windows[count].is_cacheable = true;
            count++;
        } else if (cache_mode == OBMM_IMPORT_CACHE_NC) {
            if (nc_size_mb == 0)
                continue;
            if (count >= OBMM_POOL_HELPERS_MAX_WINDOWS)
                break;
            windows[count].mar = (unsigned int)mar;
            windows[count].decode = (uint64_t)decode;
            windows[count].base_pa = ((uint64_t)nc_base_mb) << 20;
            windows[count].size_bytes = ((uint64_t)nc_size_mb) << 20;
            windows[count].is_cacheable = false;
            count++;
        } else {
            if (nc_size_mb != 0) {
                if (count >= OBMM_POOL_HELPERS_MAX_WINDOWS)
                    break;
                windows[count].mar = (unsigned int)mar;
                windows[count].decode = (uint64_t)decode;
                windows[count].base_pa = ((uint64_t)nc_base_mb) << 20;
                windows[count].size_bytes = ((uint64_t)nc_size_mb) << 20;
                windows[count].is_cacheable = false;
                count++;
            }
            if (cc_size_mb != 0) {
                if (count >= OBMM_POOL_HELPERS_MAX_WINDOWS)
                    break;
                windows[count].mar = (unsigned int)mar;
                windows[count].decode = (uint64_t)decode;
                windows[count].base_pa = ((uint64_t)cc_base_mb) << 20;
                windows[count].size_bytes = ((uint64_t)cc_size_mb) << 20;
                windows[count].is_cacheable = true;
                count++;
            }
        }
    }
    fclose(fp);
    *count_out = count;
    return count > 0;
}

static enum obmm_import_cache_mode OBMM_MAYBE_UNUSED obmm_parse_import_cache_mode(void)
{
    const char *env = getenv("OBMM_IMPORT_CACHE_MODE");
    if (!env || env[0] == '\0' || strcmp(env, "auto") == 0)
        return OBMM_IMPORT_CACHE_AUTO;
    if (strcmp(env, "nc") == 0)
        return OBMM_IMPORT_CACHE_NC;
    if (strcmp(env, "cc") == 0)
        return OBMM_IMPORT_CACHE_CC;
    fprintf(stderr, "[obmm] warn: unknown OBMM_IMPORT_CACHE_MODE=%s, using auto\n", env);
    return OBMM_IMPORT_CACHE_AUTO;
}

static bool OBMM_MAYBE_UNUSED obmm_alloc_import_pas(int import_count, uint64_t size_per_import,
                                  uint64_t pas[OBMM_POOL_HELPERS_MAX_NODES],
                                  bool osync[OBMM_POOL_HELPERS_MAX_NODES],
                                  enum obmm_import_cache_mode cache_mode)
{
    struct obmm_helpers_window windows[OBMM_POOL_HELPERS_MAX_WINDOWS];
    int window_count = 0;
    int import_idx = 0;
    int wi;
    if (import_count <= 0)
        return true;
    if (!obmm_parse_windows(windows, &window_count, cache_mode))
        return false;
    for (wi = 0; wi < window_count && import_idx < import_count; wi++) {
        uint64_t cur = obmm_align_up_u64(windows[wi].base_pa,
                                         OBMM_POOL_HELPERS_IMPORT_ALIGN);
        uint64_t end = windows[wi].base_pa + windows[wi].size_bytes;
        while (import_idx < import_count && cur + size_per_import <= end) {
            pas[import_idx] = cur;
            osync[import_idx] = !windows[wi].is_cacheable;
            import_idx++;
            cur = obmm_align_up_u64(cur + size_per_import,
                                    OBMM_POOL_HELPERS_IMPORT_ALIGN);
        }
    }
    return import_idx == import_count;
}

#endif /* OBMM_COMMON_H */
