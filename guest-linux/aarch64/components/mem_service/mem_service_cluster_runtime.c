#include "mem_service_internal.h"

#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_qwen3_runtime.h"

int mem_service_cluster_runtime_make_gsva_buffer_desc(
    const struct mem_service_cluster_runtime *rt,
    const struct mem_service_record *record,
    struct mem_service_gsva_buffer_desc *out)
{
    struct mem_service_gsva_desc_source source;
    int i;

    if (!rt) {
        return -1;
    }
    memset(&source, 0, sizeof(source));
    source.active = rt->active;
    source.node_count = rt->node_count;
    source.local_idx = rt->local_idx;
    source.local_cna = rt->local_cna;
    source.payload_offset = rt->payload_offset;
    for (i = 0; i < rt->node_count && i < (int)MEM_SERVICE_GSVA_MAX_NODES; ++i) {
        source.metas[i].segment_id = rt->metas[i].export_mem_id;
        source.metas[i].home_va = rt->metas[i].remote_uba;
        source.metas[i].region_bytes = rt->metas[i].size;
        source.metas[i].token_id = rt->metas[i].token_id;
        source.metas[i].home_cna = rt->metas[i].export_cna;
    }
    return mem_service_make_gsva_buffer_desc_from_source(&source, record, out);
}

static int mem_service_read_primary_cna(uint32_t *local_cna_out)
{
    uint64_t local_cna_u64 = 0;

    if (!local_cna_out) {
        return -1;
    }
    if (!mem_service_parse_hex_file_u64("/sys/bus/ub/devices/00001/primary_cna", &local_cna_u64)) {
        return -1;
    }
    *local_cna_out = (uint32_t)local_cna_u64;
    return 0;
}

static uint64_t mem_service_import_pa_bias(void)
{
    const char *raw = getenv("SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB");
    char *end = NULL;
    unsigned long long mb;

    if (!raw || raw[0] == '\0') {
        return 0;
    }
    errno = 0;
    mb = strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0') {
        fprintf(stderr,
                "[mem_service] warn invalid SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB=%s ignored\n",
                raw);
        return 0;
    }
    return obmm_align_up_u64((uint64_t)mb * 1024ULL * 1024ULL,
                             OBMM_POOL_HELPERS_IMPORT_ALIGN);
}

static uint64_t mem_service_bootstrap_generation(void)
{
    const char *raw = getenv("SIM_MEM_SERVICE_BOOTSTRAP_GENERATION");
    char *end = NULL;
    unsigned long long generation;

    if (!raw || raw[0] == '\0') {
        return 1;
    }
    errno = 0;
    generation = strtoull(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' || generation == 0) {
        fprintf(stderr,
                "[mem_service] warn invalid SIM_MEM_SERVICE_BOOTSTRAP_GENERATION=%s using default=1\n",
                raw);
        return 1;
    }
    return (uint64_t)generation;
}

static int mem_service_exchange_cluster_meta(struct mem_service_cluster_runtime *rt,
                                       const struct mem_service_cluster_meta *local_meta)
{
    struct obmm_helpers_meta publish_meta;
    struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
    bool got[OBMM_POOL_HELPERS_MAX_NODES];
    int i;

    memset(&publish_meta, 0, sizeof(publish_meta));
    publish_meta.export_mem_id = local_meta->export_mem_id;
    publish_meta.remote_uba = local_meta->remote_uba;
    publish_meta.size = local_meta->size;
    publish_meta.token_id = local_meta->token_id;
    publish_meta.export_cna = local_meta->export_cna;

    memset(got, 0, sizeof(got));

    if (obmm_bootstrap_publish(rt->obmm_fd, rt->local_idx, rt->node_count,
                               rt->bootstrap_generation, &publish_meta) != 0) {
        fprintf(stderr, "[mem_service] FM bootstrap publish failed: %s\n", strerror(errno));
        return -1;
    }

    if (obmm_bootstrap_lookup(rt->obmm_fd, rt->local_cna, rt->node_count,
                              rt->bootstrap_generation, peer_metas, got) != 0) {
        fprintf(stderr, "[mem_service] FM bootstrap lookup failed: %s\n", strerror(errno));
        return -1;
    }

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx) continue;
        rt->metas[i].export_mem_id = peer_metas[i].export_mem_id;
        rt->metas[i].remote_uba = peer_metas[i].remote_uba;
        rt->metas[i].size = peer_metas[i].size;
        rt->metas[i].token_id = peer_metas[i].token_id;
        rt->metas[i].export_cna = peer_metas[i].export_cna;
    }
    return 0;
}

static int mem_service_init_export_layout(struct mem_service_cluster_runtime *rt, void *base)
{
    int peer_count = rt->node_count - 1;
    uint64_t queue_size = obmm_queue_region_size(MEM_SERVICE_CLUSTER_QUEUE_DEPTH);
    uint64_t header_offset = 0;
    uint64_t dir_offset = 64;
    uint64_t dir_count = peer_count + 1;
    uint64_t queue_base = obmm_align_up_u64(dir_offset + dir_count * 32, 64);
    uint64_t payload_offset = obmm_align_up_u64(queue_base + (uint64_t)peer_count * queue_size, 64);
    struct obmm_pool_header *hdr;
    int i, peer_idx;

    hdr = (struct obmm_pool_header *)base;
    memset(hdr, 0, 64);
    hdr->magic = OBMM_POOL_MAGIC;
    hdr->layout_version = OBMM_POOL_LAYOUT_VERSION;
    hdr->node_id = (uint16_t)rt->local_idx;
    hdr->node_count = (uint16_t)rt->node_count;
    atomic_store(&hdr->state, OBMM_POOL_STATE_INIT);
    hdr->region_size = rt->region_size;
    hdr->directory_offset = dir_offset;
    hdr->directory_count = (uint32_t)dir_count;
    hdr->default_queue_depth = MEM_SERVICE_CLUSTER_QUEUE_DEPTH;

    peer_idx = 0;
    for (i = 0; i < rt->node_count; i++) {
        struct obmm_region_dirent *de;
        if (i == rt->local_idx) continue;
        de = (struct obmm_region_dirent *)((uint8_t *)base + dir_offset) + peer_idx;
        memset(de, 0, 32);
        de->region_id = (uint32_t)peer_idx;
        de->kind = OBMM_REGION_QUEUE;
        de->peer_node_id = (uint16_t)i;
        de->offset = queue_base + (uint64_t)peer_idx * queue_size;
        de->size = queue_size;

        rt->ingress_queues[i] = (struct obmm_spsc_queue *)((uint8_t *)base + de->offset);
        obmm_spsc_queue_init(rt->ingress_queues[i], MEM_SERVICE_CLUSTER_QUEUE_DEPTH);

        peer_idx++;
    }

    {
        struct obmm_region_dirent *de;
        de = (struct obmm_region_dirent *)((uint8_t *)base + dir_offset) + peer_idx;
        memset(de, 0, 32);
        de->region_id = (uint32_t)peer_idx;
        de->kind = OBMM_REGION_MEM_SERVICE_PAYLOAD;
        de->peer_node_id = (uint16_t)rt->local_idx;
        de->offset = payload_offset;
        de->size = rt->region_size - payload_offset;
    }

    rt->ingress_queue_base = base;
    atomic_store(&hdr->state, OBMM_POOL_STATE_READY);
    fprintf(stderr, "[mem_service] export layout -> ok queues=%d queue_depth=%d payload_offset=%luKB\n",
            peer_count, MEM_SERVICE_CLUSTER_QUEUE_DEPTH, (unsigned long)(payload_offset / 1024));
    (void)header_offset;
    return 0;
}

static void mem_service_cleanup_cluster_slots(struct mem_service_cluster_runtime *rt)
{
    int i;

    for (i = 0; i < rt->node_count; ++i) {
        /* Undo payload_offset adjustment for local and remote slots */
        if (rt->slots[i].region.addr && rt->payload_offset > 0 &&
            (rt->slots[i].is_local || rt->slots[i].mem_id != 0)) {
            rt->slots[i].region.addr =
                (uint8_t *)rt->slots[i].region.addr - rt->payload_offset;
            rt->slots[i].region.len = rt->region_size;
        }
        if (rt->slots[i].region.addr || rt->slots[i].region.fd >= 0) {
            obmm_unmap_region((struct obmm_helpers_region *)&rt->slots[i].region);
        }
        if (rt->slots[i].mem_id != 0) {
            if (i == rt->local_idx) {
                (void)obmm_do_unexport(rt->obmm_fd, rt->slots[i].mem_id);
            } else {
                (void)obmm_do_unimport(rt->obmm_fd, rt->slots[i].mem_id);
            }
        }
        if (rt->egress_import[i].addr || rt->egress_import[i].fd >= 0) {
            obmm_unmap_region(&rt->egress_import[i]);
        }
    }
}

static void mem_service_cluster_runtime_mark_closed(struct mem_service_cluster_runtime *rt)
{
    int i;

    if (!rt) {
        return;
    }
    rt->obmm_fd = -1;
    rt->local_idx = -1;
    for (i = 0; i < MEM_SERVICE_CLUSTER_MAX_NODES; ++i) {
        rt->slots[i].region.fd = -1;
        rt->egress_import[i].fd = -1;
    }
}

int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx)
{
    struct mem_service_cluster_slot *slot;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count || owner_idx == rt->local_idx) {
        return -1;
    }

    slot = &rt->slots[owner_idx];
    if (!slot->map_osync) {
        fprintf(stderr,
                "[mem_service] invariant violation remote_slot_map_osync_true_expected node=%d map_osync=%d\n",
                owner_idx + 1,
                slot->map_osync ? 1 : 0);
        slot->map_osync = true;
    }
    if (slot->region.addr && slot->mem_id != 0) {
        return 0;
    }
    if (slot->mem_id != 0) {
        (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
    }
    if (slot->region.addr || slot->region.fd >= 0) {
        obmm_unmap_region((struct obmm_helpers_region *)&slot->region);
    }
    {
        struct obmm_helpers_meta import_meta;
        import_meta.export_mem_id = rt->metas[owner_idx].export_mem_id;
        import_meta.remote_uba = rt->metas[owner_idx].remote_uba;
        import_meta.size = rt->metas[owner_idx].size;
        import_meta.token_id = rt->metas[owner_idx].token_id;
        import_meta.export_cna = rt->metas[owner_idx].export_cna;
        if (obmm_do_import(rt->obmm_fd, &import_meta,
                           rt->local_cna, slot->local_pa,
                           import_meta.token_id, &slot->mem_id) != 0) {
            return -1;
        }
    }
    if (obmm_map_region(slot->mem_id,
                        rt->region_size,
                        slot->map_osync,
                        (struct obmm_helpers_region *)&slot->region) != 0) {
        (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
        return -1;
    }

    /* Poll peer's pool state until READY -- ensures cacheable writes by the
     * exporter are visible through our osync import mapping before we read
     * the directory and queue structures. */
    {
        struct obmm_pool_header *phdr =
            (struct obmm_pool_header *)slot->region.addr;
        long poll_deadline = obmm_now_ms() + 90000;
        while (obmm_now_ms() < poll_deadline) {
            uint32_t st = atomic_load_explicit(&phdr->state,
                                               memory_order_acquire);
            if (st == OBMM_POOL_STATE_READY)
                break;
            usleep(1000);
        }
        if (atomic_load_explicit(
                &((struct obmm_pool_header *)slot->region.addr)->state,
                memory_order_acquire) != OBMM_POOL_STATE_READY) {
            fprintf(stderr, "[mem_service] peer node%d pool not READY\n",
                    owner_idx + 1);
            obmm_unmap_region((struct obmm_helpers_region *)&slot->region);
            (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
            slot->mem_id = 0;
            return -1;
        }
    }

    /* Resolve egress queue (remote node's ingress queue for us) from directory */
    if (rt->egress_queues[owner_idx] == NULL && slot->region.addr != NULL) {
        struct obmm_pool_header *hdr = (struct obmm_pool_header *)slot->region.addr;
        struct obmm_region_dirent *dir = (struct obmm_region_dirent *)
            ((uint8_t *)slot->region.addr + hdr->directory_offset);
        int d;
        for (d = 0; (uint32_t)d < hdr->directory_count; d++) {
            if (dir[d].kind == OBMM_REGION_QUEUE &&
                dir[d].peer_node_id == (uint16_t)rt->local_idx) {
                rt->egress_queues[owner_idx] = (struct obmm_spsc_queue *)
                    ((uint8_t *)slot->region.addr + dir[d].offset);
                break;
            }
        }
    }

    /* Adjust slot's region.addr to point at the payload sub-region */
    if (rt->payload_offset > 0 && slot->region.addr != NULL) {
        slot->region.addr = (uint8_t *)slot->region.addr + rt->payload_offset;
        slot->region.len = rt->region_size - rt->payload_offset;
    }

    return 0;
}

static void mem_service_release_remote_slot(struct mem_service_cluster_runtime *rt,
                                            int owner_idx)
{
    struct mem_service_cluster_slot *slot;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count ||
        owner_idx == rt->local_idx) {
        return;
    }
    slot = &rt->slots[owner_idx];
    rt->egress_queues[owner_idx] = NULL;
    if (slot->region.addr && rt->payload_offset > 0) {
        slot->region.addr = (uint8_t *)slot->region.addr - rt->payload_offset;
        slot->region.len = rt->region_size;
    }
    if (slot->region.addr || slot->region.fd >= 0) {
        obmm_unmap_region((struct obmm_helpers_region *)&slot->region);
    }
    if (slot->mem_id != 0) {
        (void)obmm_do_unimport(rt->obmm_fd, slot->mem_id);
        slot->mem_id = 0;
    }
}

int mem_service_refresh_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx)
{
    struct obmm_helpers_meta peer_metas[OBMM_POOL_HELPERS_MAX_NODES];
    bool got[OBMM_POOL_HELPERS_MAX_NODES];

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count ||
        owner_idx == rt->local_idx) {
        return -1;
    }
    memset(peer_metas, 0, sizeof(peer_metas));
    memset(got, 0, sizeof(got));
    if (obmm_bootstrap_lookup(rt->obmm_fd, rt->local_cna, rt->node_count,
                              rt->bootstrap_generation, peer_metas, got) != 0 ||
        !got[owner_idx]) {
        return -1;
    }
    rt->metas[owner_idx].export_mem_id = peer_metas[owner_idx].export_mem_id;
    rt->metas[owner_idx].remote_uba = peer_metas[owner_idx].remote_uba;
    rt->metas[owner_idx].size = peer_metas[owner_idx].size;
    rt->metas[owner_idx].token_id = peer_metas[owner_idx].token_id;
    rt->metas[owner_idx].export_cna = peer_metas[owner_idx].export_cna;
    mem_service_release_remote_slot(rt, owner_idx);
    return mem_service_activate_remote_slot(rt, owner_idx);
}

static void mem_service_cluster_runtime_reset(struct mem_service_cluster_runtime *rt)
{
    if (!rt) {
        return;
    }
    if (rt->obmm_fd >= 0) {
        mem_service_cleanup_cluster_slots(rt);
        close(rt->obmm_fd);
    }
    memset(rt, 0, sizeof(*rt));
    mem_service_cluster_runtime_mark_closed(rt);
    rt->payload_arena_base = 0;
    rt->payload_arena_next = 0;
    rt->payload_arena_high_water = 0;
    rt->bootstrap_generation = 1;
    rt->pool_layout_reported = false;
}

void mem_service_cluster_runtime_destroy(struct mem_service_cluster_runtime *rt)
{
    mem_service_cluster_runtime_reset(rt);
}

int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt)
{
    if (!rt || !rt->active || rt->local_idx < 0 || rt->node_count <= 0 ||
        rt->local_idx >= rt->node_count ||
        !rt->slots[rt->local_idx].region.addr) {
        printf("[mem_service] gap db_service_cluster_stage=runtime_not_bootstrapped\n");
        return -1;
    }
    return 0;
}

int mem_service_cluster_runtime_init(struct mem_service_cluster_runtime *rt)
{
    char local_ip[INET_ADDRSTRLEN];
    char ips[MEM_SERVICE_CLUSTER_MAX_NODES][INET_ADDRSTRLEN];
    struct mem_service_cluster_meta local_meta;
    struct obmm_helpers_meta export_meta;
    uint64_t import_pas[MEM_SERVICE_CLUSTER_MAX_NODES];
    bool import_osync[MEM_SERVICE_CLUSTER_MAX_NODES];
    int import_count;
    int import_idx;
    uint64_t import_pa_bias;
    char region_size_str[32];
    uint64_t region_size_mb;
    uint64_t payload_offset;
    int i;

    if (!rt) {
        return -1;
    }
    if (rt->active) {
        return 0;
    }
    mem_service_cluster_runtime_reset(rt);
    rt->lazy_remote_activation =
        getenv("SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION") != NULL &&
        strcmp(getenv("SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION"), "1") == 0;
    memset(&local_meta, 0, sizeof(local_meta));

    if (!mem_service_resolve_cluster_nodes(local_ip, ips, &rt->node_count, &rt->local_idx)) {
        return -1;
    }

    /* Read region size from /proc/cmdline, default to MEM_SERVICE_DEFAULT_REGION_SIZE_MB */
    if (obmm_cmdline_get(MEM_SERVICE_CMDLINE_REGION_SIZE, region_size_str, sizeof(region_size_str))) {
        errno = 0;
        region_size_mb = (uint64_t)strtoull(region_size_str, NULL, 0);
        if (errno != 0 || region_size_mb == 0) {
            region_size_mb = MEM_SERVICE_DEFAULT_REGION_SIZE_MB;
        }
    } else {
        region_size_mb = MEM_SERVICE_DEFAULT_REGION_SIZE_MB;
    }
    rt->region_size = obmm_align_up_u64(region_size_mb * 1024ULL * 1024ULL,
                                         OBMM_POOL_HELPERS_IMPORT_ALIGN);
    rt->bootstrap_generation = mem_service_bootstrap_generation();
    fprintf(stderr, "[mem_service] region_size=%luMB (aligned=%luMB)\n",
            (unsigned long)region_size_mb,
            (unsigned long)(rt->region_size / (1024ULL * 1024ULL)));
    fprintf(stderr,
            "[mem_service] bootstrap_generation=%" PRIu64 "\n",
            rt->bootstrap_generation);

    rt->obmm_fd = obmm_open_device();
    if (rt->obmm_fd < 0) {
        goto fail;
    }
    if (mem_service_read_primary_cna(&rt->local_cna) != 0) {
        goto fail;
    }
    local_meta.export_cna = rt->local_cna;

    /* Export local region */
    memset(&export_meta, 0, sizeof(export_meta));
    export_meta.export_cna = rt->local_cna;
    if (obmm_do_export(rt->obmm_fd, &export_meta, rt->region_size) != 0) {
        goto fail;
    }
    local_meta.export_mem_id = export_meta.export_mem_id;
    local_meta.remote_uba = export_meta.remote_uba;
    local_meta.size = export_meta.size;
    local_meta.token_id = export_meta.token_id;
    local_meta.export_cna = export_meta.export_cna;
    rt->metas[rt->local_idx] = local_meta;

    rt->slots[rt->local_idx].owner_idx = rt->local_idx;
    rt->slots[rt->local_idx].reader_idx = rt->local_idx;
    rt->slots[rt->local_idx].is_local = true;
    rt->slots[rt->local_idx].mem_id = local_meta.export_mem_id;
    rt->slots[rt->local_idx].export_cna = rt->local_cna;
    if (obmm_map_region(local_meta.export_mem_id,
                        rt->region_size,
                        false,
                        (struct obmm_helpers_region *)&rt->slots[rt->local_idx].region) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=map_local_failed mem_id=%" PRIu64 "\n",
               local_meta.export_mem_id);
        goto fail;
    }

    /* Initialize export layout with queues and payload region */
    if (mem_service_init_export_layout(rt, rt->slots[rt->local_idx].region.addr) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=export_layout_failed\n");
        goto fail;
    }

    /* Find payload offset from directory */
    payload_offset = 0;
    {
        struct obmm_pool_header *hdr = (struct obmm_pool_header *)rt->slots[rt->local_idx].region.addr;
        struct obmm_region_dirent *dir = (struct obmm_region_dirent *)
            ((uint8_t *)rt->slots[rt->local_idx].region.addr + hdr->directory_offset);
        for (i = 0; (uint32_t)i < hdr->directory_count; i++) {
            if (dir[i].kind == OBMM_REGION_MEM_SERVICE_PAYLOAD) {
                payload_offset = dir[i].offset;
                break;
            }
        }
    }
    if (payload_offset == 0) {
        printf("[mem_service] gap db_service_cluster_stage=no_payload_entry\n");
        goto fail;
    }

    if (mem_service_update_region_range_at(&rt->slots[rt->local_idx],
                                     0,
                                     payload_offset,
                                     true) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=publish_pool_layout_failed\n");
        goto fail;
    }
    (void)msync(rt->slots[rt->local_idx].region.addr,
                (size_t)payload_offset,
                MS_SYNC);

    /* Adjust local slot's region.addr to point at the payload sub-region */
    rt->payload_offset = payload_offset;
    rt->slots[rt->local_idx].region.addr =
        (uint8_t *)rt->slots[rt->local_idx].region.addr + payload_offset;
    rt->slots[rt->local_idx].region.len = rt->region_size - payload_offset;
    rt->payload_arena_base =
        obmm_align_up_u64(MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET, 64);
    rt->payload_arena_next = rt->payload_arena_base;
    rt->payload_arena_high_water = rt->payload_arena_base;
    mem_service_report_obmm_pool_layout_once(rt);

    /* FM bootstrap for peer discovery */
    if (mem_service_exchange_cluster_meta(rt, &local_meta) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=hello_timeout\n");
        goto fail;
    }

    /* Allocate import PAs for peer regions */
    import_count = rt->node_count - 1;
    if (!obmm_alloc_import_pas(import_count, rt->region_size, import_pas, import_osync,
                               obmm_parse_import_cache_mode())) {
        printf("[mem_service] gap db_service_cluster_stage=import_alloc_failed count=%d\n",
               import_count);
        goto fail;
    }
    import_pa_bias = mem_service_import_pa_bias();
    if (import_pa_bias != 0) {
        for (i = 0; i < import_count; ++i) {
            import_pas[i] += import_pa_bias;
        }
        fprintf(stderr,
                "[mem_service] import_pa_bias_mb=%lu slots=%d\n",
                (unsigned long)(import_pa_bias / (1024ULL * 1024ULL)),
                import_count);
    }

    import_idx = 0;
    for (i = 0; i < rt->node_count; ++i) {
        if (i == rt->local_idx) {
            continue;
        }
        rt->slots[i].owner_idx = i;
        rt->slots[i].reader_idx = rt->local_idx;
        rt->slots[i].is_local = false;
        rt->slots[i].local_pa = import_pas[import_idx];
        rt->slots[i].map_osync = true;
        fprintf(stderr,
                "[mem_service] remote_slot_map_osync_forced node=%d map_osync=%d\n",
                i + 1,
                rt->slots[i].map_osync ? 1 : 0);
        rt->slots[i].export_cna = rt->metas[i].export_cna;
        import_idx += 1;
        rt->slots[i].mem_id = 0;
        memset(&rt->slots[i].region, 0, sizeof(rt->slots[i].region));
        rt->slots[i].region.fd = -1;

        if (!rt->lazy_remote_activation) {
            /* Import, map, and resolve egress queue for this peer now so that
             * SPSC queue barriers can push descriptors immediately. */
            if (mem_service_activate_remote_slot(rt, i) != 0) {
                printf("[mem_service] gap db_service_cluster_stage=activate_remote_failed owner=node%d\n",
                       i + 1);
                goto fail;
            }
        }
    }

    rt->active = true;
    if (rt->lazy_remote_activation) {
        printf("[mem_service] stage db_service_cluster=local_pool_ready node=%d peers=%d activation=lazy backing=obmm_pool queue=obmm_spsc status=ok\n",
               rt->local_idx + 1,
               rt->node_count - 1);
    }
    return 0;

fail:
    mem_service_cluster_runtime_reset(rt);
    return -1;
}
