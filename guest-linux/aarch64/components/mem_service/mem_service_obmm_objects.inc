static void mem_service_fill_obmm_object_payload(uint8_t *dst,
                                           uint64_t len,
                                           uint32_t owner_node,
                                           uint32_t payload_kind)
{
    uint64_t i;

    for (i = 0; i < len; ++i) {
        dst[i] = (uint8_t)((i * 17ULL + (uint64_t)(owner_node + 1U) * 29ULL +
                            (uint64_t)payload_kind * 53ULL) & 0xffU);
    }
    if (len >= 4104U) {
        memcpy(dst + 0, "MSOBMM00", 8);
        memcpy(dst + 248, "MSOBMM248", 9);
        memcpy(dst + 256, "MSOBMM256", 9);
        memcpy(dst + 4088, "MSOBMM4088", 10);
        memcpy(dst + 4096, "MSOBMM4096", 10);
    }
}

static const char *mem_service_object_kind_name(uint32_t payload_kind)
{
    switch (payload_kind) {
    case MEM_SERVICE_OBMM_KIND_WEIGHT_TILE:
        return "weight_tile";
    case MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK:
        return "kvcache_block";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT:
        return "hidden_range_input";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT:
        return "hidden_range_output";
    case MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT:
        return "hidden_range_runtime_output";
    case MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT:
        return "qwen3_token_result";
    case MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE:
        return "qwen3_kv_state";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY:
        return "qwen3_engram_history";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES:
        return "qwen3_engram_candidates";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED:
        return "qwen3_engram_selected";
    case MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE:
        return "qwen3_engram_state";
    default:
        return "unknown";
    }
}

static int mem_service_payload_arena_alloc(struct mem_service_cluster_runtime *rt,
                                     uint64_t bytes,
                                     uint64_t align,
                                     uint64_t *offset_out)
{
    uint64_t offset;
    uint64_t end;

    if (!rt || !offset_out || rt->local_idx < 0 ||
        rt->local_idx >= rt->node_count || bytes == 0) {
        return -1;
    }
    if (align == 0) {
        align = 64;
    }
    if (rt->payload_arena_base == 0) {
        rt->payload_arena_base =
            obmm_align_up_u64(MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET, align);
        rt->payload_arena_next = rt->payload_arena_base;
        rt->payload_arena_high_water = rt->payload_arena_base;
    }
    offset = obmm_align_up_u64(rt->payload_arena_next, align);
    end = offset + bytes;
    if (end < offset ||
        !rt->slots[rt->local_idx].region.addr ||
        end > rt->slots[rt->local_idx].region.len) {
        printf("[mem_service] gap obmm_pool_allocator=exhausted local=node%d offset=0x%016" PRIx64
               " bytes=%" PRIu64 " payload_bytes=%zu arena_base=0x%016" PRIx64 "\n",
               rt->local_idx + 1,
               offset,
               bytes,
               rt->slots[rt->local_idx].region.len,
               rt->payload_arena_base);
        return -1;
    }
    rt->payload_arena_next = end;
    if (end > rt->payload_arena_high_water) {
        rt->payload_arena_high_water = end;
    }
    *offset_out = offset;
    return 0;
}

static int mem_service_put_obmm_object_record(struct mem_service *svc,
                                        enum mem_service_record_kind record_kind,
                                        const char *key,
                                        uint32_t owner_node,
                                        uint32_t payload_kind,
                                        uint64_t offset,
                                        uint64_t len,
                                        uint64_t checksum,
                                        struct mem_service_record *resolved_out)
{
    struct mem_service_record *rec;
    uint64_t next_version = 1U;

    if (!svc || !key || len == 0) {
        return -1;
    }
    rec = mem_service_find_record(svc, key);
    if (rec && rec->version != UINT64_MAX) {
        next_version = rec->version + 1U;
    }
    if (!rec) {
        rec = mem_service_alloc_record(svc);
    }
    if (!rec) {
        rec = mem_service_recycle_qwen3_runtime_record(svc, key);
    }
    if (!rec) {
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = record_kind;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    rec->placement_node = owner_node;
    rec->placement_level = 2U;
    rec->hot_segment_id = offset;
    rec->state = MEM_SERVICE_KVCACHE_STATE_HOT;
    rec->version = next_version;
    rec->last_result_segment = offset + len;
    rec->object_owner_node = owner_node;
    rec->object_payload_kind = payload_kind;
    rec->object_backing_offset = offset;
    rec->object_backing_len = len;
    rec->object_payload_checksum = checksum;
    if (resolved_out) {
        memcpy(resolved_out, rec, sizeof(*resolved_out));
    }
    return 0;
}
