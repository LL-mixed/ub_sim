static long mem_service_wallclock_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return 0;
    }
    return (long)(ts.tv_sec * 1000L + ts.tv_nsec / 1000000L);
}

static void mem_service_cpu_relax_wait(unsigned int *attempt)
{
    struct timespec ts;
    unsigned int step = attempt ? *attempt : 0;
    long usec = 1000L;

    if (step < 32U) {
        usec <<= step / 4U;
    } else {
        usec = 64000L;
    }
    if (usec > 64000L) {
        usec = 64000L;
    }
    ts.tv_sec = usec / 1000000L;
    ts.tv_nsec = (usec % 1000000L) * 1000L;
    (void)nanosleep(&ts, NULL);
    if (attempt && *attempt < 64U) {
        *attempt += 1U;
    }
}

static bool mem_service_parse_ip_list(const char *csv,
                                char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                int *count_out)
{
    char copy[256];
    char *saveptr = NULL;
    char *tok = NULL;
    int count = 0;

    if (!csv || !count_out) {
        return false;
    }
    snprintf(copy, sizeof(copy), "%s", csv);
    tok = strtok_r(copy, ",", &saveptr);
    while (tok && count < MEM_SERVICE_CLUSTER_MAX_NODES) {
        snprintf(ips[count], INET_ADDRSTRLEN, "%s", tok);
        count += 1;
        tok = strtok_r(NULL, ",", &saveptr);
    }
    if (count < 2) {
        return false;
    }
    *count_out = count;
    return true;
}

static bool mem_service_resolve_cluster_nodes(char local_ip[INET_ADDRSTRLEN],
                                        char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN],
                                        int *node_count,
                                        int *local_idx)
{
    const char *env_local = getenv("LINQU_UB_LOCAL_IP");
    const char *env_all = getenv("LINQU_UB_ALL_IPS");
    int i;

    if (!env_local || !env_all) {
        return false;
    }
    snprintf(local_ip, INET_ADDRSTRLEN, "%s", env_local);
    if (!mem_service_parse_ip_list(env_all, ips, node_count)) {
        return false;
    }
    for (i = 0; i < *node_count; ++i) {
        if (strcmp(ips[i], local_ip) == 0) {
            *local_idx = i;
            return true;
        }
    }
    return false;
}

static bool mem_service_parse_hex_file_u64(const char *path, uint64_t *value)
{
    char buf[256];
    char *end = NULL;
    unsigned long long v;
    FILE *fp = fopen(path, "r");

    if (!fp) {
        return false;
    }
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return false;
    }
    fclose(fp);
    errno = 0;
    v = strtoull(buf, &end, 0);
    if (errno != 0 || end == buf) {
        return false;
    }
    *value = (uint64_t)v;
    return true;
}

static int mem_service_update_region_range_at(const struct mem_service_cluster_slot *slot,
                                        uint64_t offset,
                                        uint64_t length,
                                        bool for_write)
{
    struct obmm_cmd_update_range cmd;
    uintptr_t start;
    uintptr_t end;
    uintptr_t page_size;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (slot->map_osync) {
        return 0;
    }
    if (length == 0 || offset + length > slot->region.len) {
        return -1;
    }
    start = (uintptr_t)slot->region.addr + (uintptr_t)offset;
    end = start + (uintptr_t)length;
    page_size = (uintptr_t)sysconf(_SC_PAGESIZE);
    if (page_size == 0) {
        page_size = 4096;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.start = start & ~(uintptr_t)(page_size - 1);
    cmd.end = (end + page_size - 1) & ~(uintptr_t)(page_size - 1);
    cmd.mem_state = (slot->map_osync ? OBMM_SHM_MEM_NORMAL_NC : OBMM_SHM_MEM_NORMAL) |
                    OBMM_SHM_MEM_READWRITE;
    cmd.cache_ops = for_write ? OBMM_SHM_CACHE_WB_INVAL : OBMM_SHM_CACHE_INVAL;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_UPDATE_RANGE, &cmd) == 0) {
        return 0;
    }
    fprintf(stderr,
            "[mem_service] update_range_failed owner=%d write=%d fd=%d start=%#llx end=%#llx errno=%d\n",
            slot->owner_idx + 1, for_write ? 1 : 0, slot->region.fd,
            (unsigned long long)cmd.start, (unsigned long long)cmd.end, errno);
    return -1;
}

static int mem_service_update_region_range(const struct mem_service_cluster_slot *slot, bool for_write)
{
    return mem_service_update_region_range_at(slot, 0, sizeof(struct mem_service_cluster_payload), for_write);
}

static int MEM_SERVICE_MAYBE_UNUSED mem_service_sync_remote_range(
    const struct mem_service_cluster_slot *slot,
    uint64_t offset,
    uint64_t length)
{
    obmm_cmd_sync_remote_range cmd;
    struct stat st;
    char fd_path[64];
    char fd_target[256];
    ssize_t n;

    if (!slot || !slot->region.addr || slot->region.fd < 0) {
        return -1;
    }
    if (!slot->map_osync || slot->is_local) {
        return 0;
    }
    if (length == 0) {
        return 0;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.offset = offset;
    cmd.length = length;
    if (ioctl(slot->region.fd, OBMM_SHMDEV_SYNC_REMOTE_RANGE, &cmd) == 0) {
        return 0;
    }
    fd_target[0] = '\0';
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", slot->region.fd);
    n = readlink(fd_path, fd_target, sizeof(fd_target) - 1);
    if (n > 0) {
        fd_target[n] = '\0';
    } else {
        snprintf(fd_target, sizeof(fd_target), "<readlink:%s>", strerror(errno));
    }
    fprintf(stderr,
            "[mem_service] sync_remote_range_failed owner=%d fd=%d target=%s offset=%#" PRIx64
            " len=%#" PRIx64 " errno=%d",
            slot->owner_idx + 1,
            slot->region.fd,
            fd_target,
            offset,
            length,
            errno);
    if (fstat(slot->region.fd, &st) == 0) {
        fprintf(stderr,
                " mode=%#o rdev=%u:%u",
                st.st_mode,
                major(st.st_rdev),
                minor(st.st_rdev));
    }
    fputc('\n', stderr);
    return -1;
}
