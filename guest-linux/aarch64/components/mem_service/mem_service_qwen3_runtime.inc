#include "mem_service_record_table.h"

uint64_t mem_service_qwen3_hidden_payload_checksum(const uint8_t *bytes,
                                                   uint64_t len)
{
    uint64_t acc = 0xcbf29ce484222325ULL;
    uint64_t index;

    for (index = 0; index < len; ++index) {
        acc ^= (uint64_t)bytes[index] | (index << 8);
        acc *= 0x00000100000001b3ULL;
    }
    return acc;
}

static int mem_service_qwen3_kv_state_block_span(uint64_t payload_len,
                                           uint64_t *block_bytes_out,
                                           uint64_t *block_count_out,
                                           uint64_t *reserved_bytes_out)
{
    uint64_t block_bytes;
    uint64_t block_count;
    uint64_t reserved_bytes;

    if (!block_bytes_out || !block_count_out || !reserved_bytes_out ||
        payload_len == 0) {
        return -1;
    }
    if (payload_len <= MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES) {
        block_bytes = MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES;
        block_count = 1U;
    } else if (payload_len <= MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES) {
        block_bytes = MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES;
        block_count = 1U;
    } else if (payload_len <= MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES) {
        block_bytes = MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES;
        block_count = 1U;
    } else if (payload_len <= MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES) {
        block_bytes = MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES;
        block_count = 1U;
    } else {
        block_bytes = MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES;
        block_count =
            (payload_len + block_bytes - 1U) / block_bytes;
    }
    if (block_count == 0 || block_count > UINT64_MAX / block_bytes) {
        return -1;
    }
    reserved_bytes = block_count * block_bytes;
    if (reserved_bytes < payload_len) {
        return -1;
    }
    *block_bytes_out = block_bytes;
    *block_count_out = block_count;
    *reserved_bytes_out = reserved_bytes;
    return 0;
}

int mem_service_qwen3_kv_state_alloc(struct mem_service_cluster_runtime *rt,
                                     uint64_t payload_len,
                                     uint64_t *offset_out,
                                     uint64_t *block_bytes_out,
                                     uint64_t *block_count_out,
                                     uint64_t *reserved_bytes_out)
{
    uint64_t block_bytes = 0;
    uint64_t block_count = 0;
    uint64_t reserved_bytes = 0;

    if (!offset_out ||
        mem_service_qwen3_kv_state_block_span(payload_len,
                                        &block_bytes,
                                        &block_count,
                                        &reserved_bytes) != 0) {
        return -1;
    }
    if (block_bytes_out) {
        *block_bytes_out = block_bytes;
    }
    if (block_count_out) {
        *block_count_out = block_count;
    }
    if (reserved_bytes_out) {
        *reserved_bytes_out = reserved_bytes;
    }
    return mem_service_payload_arena_alloc(rt,
                                     reserved_bytes,
                                     block_bytes,
                                     offset_out);
}

void mem_service_report_obmm_pool_layout_once(struct mem_service_cluster_runtime *rt)
{
    struct mem_service_cluster_slot *local_slot;

    if (!rt || rt->pool_layout_reported || rt->local_idx < 0 ||
        rt->local_idx >= rt->node_count) {
        return;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr) {
        return;
    }
    printf("[mem_service] stage qwen3_obmm_pool_layout local=node%d nodes=%d per_node_region_bytes=%" PRIu64
           " cluster_region_bytes=%" PRIu64 " payload_offset=%" PRIu64
           " payload_bytes=%zu arena_base=0x%016" PRIx64
           " allocator=linear_payload_arena status=ok\n",
           rt->local_idx + 1,
           rt->node_count,
           rt->region_size,
           rt->region_size * (uint64_t)rt->node_count,
           rt->payload_offset,
           local_slot->region.len,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET);
    rt->pool_layout_reported = true;
}

void mem_service_report_obmm_pool_usage(struct mem_service_cluster_runtime *rt,
                                        uint32_t local_node,
                                        uint64_t decode_step)
{
    struct mem_service_cluster_slot *local_slot;
    uint64_t arena_used;
    uint64_t payload_used;

    if (!rt || local_node >= (uint32_t)rt->node_count ||
        rt->local_idx < 0 || rt->local_idx >= rt->node_count) {
        return;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr) {
        return;
    }
    arena_used =
        rt->payload_arena_high_water > rt->payload_arena_base ?
            rt->payload_arena_high_water - rt->payload_arena_base :
            0;
    payload_used = rt->payload_arena_high_water;
    printf("[mem_service] stage qwen3_obmm_pool_usage local=node%u step=%" PRIu64
           " per_node_region_bytes=%" PRIu64 " cluster_region_bytes=%" PRIu64
           " payload_bytes=%zu payload_high_water_bytes=%" PRIu64
           " payload_used_pct_milli=%" PRIu64 " arena_base=0x%016" PRIx64
           " arena_used_bytes=%" PRIu64 " arena_next=0x%016" PRIx64
           " allocator=linear_payload_arena status=ok\n",
           local_node + 1U,
           decode_step,
           rt->region_size,
           rt->region_size * (uint64_t)rt->node_count,
           local_slot->region.len,
           payload_used,
           local_slot->region.len > 0 ?
               (uint64_t)((payload_used * 100000ULL) /
                          (uint64_t)local_slot->region.len) :
               (uint64_t)0,
           rt->payload_arena_base,
           arena_used,
           rt->payload_arena_next);
}

int mem_service_qwen3_engram_owner_index(uint32_t cluster_node_count)
{
    const char *env = getenv("SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE");
    char *end = NULL;
    unsigned long parsed;

    if (cluster_node_count == 0) {
        return -1;
    }
    if (!env || !*env) {
        return (int)cluster_node_count - 1;
    }
    errno = 0;
    parsed = strtoul(env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' ||
        parsed == 0 || parsed > cluster_node_count) {
        return (int)cluster_node_count - 1;
    }
    return (int)parsed - 1;
}

static void mem_service_qwen3_engram_session_id(char *out, size_t out_len)
{
    const char *env = getenv("SIM_QWEN3_GUEST_ENGRAM_SESSION_ID");
    size_t len;

    if (!out || out_len == 0) {
        return;
    }
    if (!env || !*env) {
        snprintf(out, out_len, "guest");
        return;
    }
    len = strlen(env);
    if (len == 0 || len >= out_len) {
        snprintf(out, out_len, "guest");
        return;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = env[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F') ||
              c == '_' || c == '-')) {
            snprintf(out, out_len, "guest");
            return;
        }
    }
    snprintf(out, out_len, "%s", env);
}

void mem_service_qwen3_engram_history_key(char *out, size_t out_len)
{
    char session[32];

    mem_service_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out, out_len, "qwen3/session/%s/tokens/history", session);
}

void mem_service_qwen3_engram_candidates_key(uint64_t decode_step,
                                              char *out,
                                              size_t out_len)
{
    char session[32];

    mem_service_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/candidates/topk",
             session,
             decode_step);
}

void mem_service_qwen3_engram_selected_key(uint64_t decode_step,
                                            char *out,
                                            size_t out_len)
{
    char session[32];

    mem_service_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/tokens/selected",
             session,
             decode_step);
}

void mem_service_qwen3_engram_state_key(uint64_t decode_step,
                                         char *out,
                                         size_t out_len)
{
    char session[32];

    mem_service_qwen3_engram_session_id(session, sizeof(session));
    snprintf(out,
             out_len,
             "qwen3/session/%s/step/%" PRIu64 "/engram/state",
             session,
             decode_step);
}

static uint64_t mem_service_qwen3_placement_checksum(uint32_t owner,
                                               uint32_t start,
                                               uint32_t end,
                                               uint32_t next_owner,
                                               bool terminal)
{
    uint64_t hash = 1469598103934665603ULL;

    hash ^= owner + 1U;
    hash *= 1099511628211ULL;
    hash ^= start;
    hash *= 1099511628211ULL;
    hash ^= end;
    hash *= 1099511628211ULL;
    hash ^= next_owner + 1U;
    hash *= 1099511628211ULL;
    hash ^= terminal ? 1U : 0U;
    hash *= 1099511628211ULL;
    return hash;
}

static void mem_service_qwen3_placement_key(uint32_t owner_node,
                                      char *out,
                                      size_t out_len)
{
    snprintf(out,
             out_len,
             "placement/%s/layer-range/node%u",
             mem_service_qwen3_model_key(),
             owner_node + 1U);
}

int mem_service_qwen3_decode_entry_node(uint32_t cluster_node_count,
                                        uint32_t *node_out)
{
    uint32_t i;

    if (!node_out || cluster_node_count != mem_service_qwen3_range_nodes()) {
        return -1;
    }
    for (i = 0; i < cluster_node_count; ++i) {
        uint32_t layer_start = 0;
        uint32_t layer_end = 0;
        uint32_t next_node = 0;

        if (mem_service_qwen3_layer_range_for_node(i,
                                             cluster_node_count,
                                             &layer_start,
                                             &layer_end,
                                             &next_node) != 0) {
            return -1;
        }
        if (layer_start == 0 && layer_end > layer_start) {
            *node_out = i;
            return 0;
        }
    }
    return -1;
}

static int mem_service_put_qwen3_layer_range_placement(
    struct mem_service *svc,
    const struct mem_service_qwen3_layer_range_placement *placement)
{
    struct mem_service_record *rec;
    char key[96];

    if (!svc || !placement || placement->layer_start >= placement->layer_end) {
        return -1;
    }
    mem_service_qwen3_placement_key(placement->owner_node, key, sizeof(key));
    rec = mem_service_find_record(svc, key);
    if (!rec) {
        rec = mem_service_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }
    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    rec->placement_node = placement->owner_node;
    rec->placement_level = 2U;
    rec->hot_segment_id = placement->layer_start;
    rec->state = MEM_SERVICE_KVCACHE_STATE_HOT;
    rec->version = 1U;
    rec->last_result_segment = placement->layer_end;
    rec->object_owner_node = placement->next_owner_node;
    rec->object_backing_len = placement->layer_count;
    rec->object_payload_checksum =
        mem_service_qwen3_placement_checksum(placement->owner_node,
                                       placement->layer_start,
                                       placement->layer_end,
                                       placement->next_owner_node,
                                       placement->terminal);
    return 0;
}

int mem_service_publish_qwen3_layer_range_placements(
    struct mem_service *svc,
    uint32_t node_count)
{
    uint32_t i;

    if (!svc || node_count != mem_service_qwen3_range_nodes()) {
        return -1;
    }
    for (i = 0; i < node_count; ++i) {
        struct mem_service_qwen3_layer_range_placement placement;

        memset(&placement, 0, sizeof(placement));
        placement.owner_node = i;
        placement.next_owner_node = (i + 1U) % node_count;
        placement.terminal = (i + 1U == node_count);
        mem_service_qwen3_node_range(i,
                               node_count,
                               &placement.layer_start,
                               &placement.layer_end);
        placement.layer_count = placement.layer_end - placement.layer_start;
        if (mem_service_put_qwen3_layer_range_placement(svc, &placement) != 0) {
            return -1;
        }
    }
    return 0;
}

bool mem_service_read_qwen3_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement)
{
    struct mem_service_record rec;
    char key[96];

    if (!svc || !placement) {
        return false;
    }
    mem_service_qwen3_placement_key(owner_node, key, sizeof(key));
    if (mem_service_get_record(svc, key, &rec) != 0 ||
        rec.kind != MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT ||
        rec.hot_segment_id >= rec.last_result_segment ||
        rec.object_backing_len != rec.last_result_segment - rec.hot_segment_id) {
        return false;
    }
    memset(placement, 0, sizeof(*placement));
    placement->owner_node = rec.placement_node;
    placement->layer_start = (uint32_t)rec.hot_segment_id;
    placement->layer_end = (uint32_t)rec.last_result_segment;
    placement->next_owner_node = rec.object_owner_node;
    placement->layer_count = (uint32_t)rec.object_backing_len;
    placement->terminal = placement->next_owner_node < placement->owner_node;
    return rec.object_payload_checksum ==
           mem_service_qwen3_placement_checksum(placement->owner_node,
                                          placement->layer_start,
                                          placement->layer_end,
                                          placement->next_owner_node,
                                          placement->terminal);
}

bool mem_service_find_qwen3_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_qwen3_layer_range_placement *placement)
{
    uint32_t i;

    if (!svc || !placement) {
        return false;
    }
    for (i = 0; i < mem_service_qwen3_range_nodes(); ++i) {
        struct mem_service_qwen3_layer_range_placement candidate;

        if (!mem_service_read_qwen3_layer_range_placement(svc, i, &candidate)) {
            return false;
        }
        if (!candidate.terminal && candidate.next_owner_node == owner_node) {
            *placement = candidate;
            return true;
        }
    }
    return false;
}
