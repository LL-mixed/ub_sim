#include "mem_service_internal.h"

#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"
#include "mem_service_ub_ssd_gsva_io.h"

static void mem_service_qwen3_format_kv_state_key(
    char *key,
    size_t key_len,
    uint32_t local_node,
    const struct mem_service_qwen3_layer_range_placement *placement,
    uint64_t decode_step)
{
    uint64_t run_scope_hash = mem_service_run_scope_hash_from_env();
    uint64_t object_decode_step =
        mem_service_serving_decode_step_from_env(decode_step);

    if (!key || key_len == 0 || !placement) {
        return;
    }
    if (run_scope_hash != 0) {
        snprintf(key,
                 key_len,
                 "kvcache/%s/scope/%016" PRIx64 "/node%u/layers-%u-%u/decode-step%" PRIu64,
                 mem_service_qwen3_model_key(),
                 run_scope_hash,
                 local_node + 1U,
                 placement->layer_start,
                 placement->layer_end,
                 object_decode_step);
        return;
    }
    snprintf(key,
             key_len,
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             placement->layer_start,
             placement->layer_end,
             object_decode_step);
}

static uint64_t mem_service_qwen3_kv_backend_key_hash(const char *key)
{
    uint64_t hash = 1469598103934665603ULL;

    if (!key) {
        return hash;
    }
    while (*key) {
        hash ^= (unsigned char)*key++;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static const char *mem_service_qwen3_kv_ub_ssd_status_name(
    enum mem_service_ub_ssd_gsva_io_status status)
{
    switch (status) {
    case MEM_SERVICE_UB_SSD_GSVA_IO_OK:
        return "ok";
    case MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED:
        return "unsupported";
    case MEM_SERVICE_UB_SSD_GSVA_IO_INVALID:
        return "invalid";
    case MEM_SERVICE_UB_SSD_GSVA_IO_STALE_REF:
        return "stale_ref";
    case MEM_SERVICE_UB_SSD_GSVA_IO_TIMEOUT:
        return "timeout";
    case MEM_SERVICE_UB_SSD_GSVA_IO_CHECKSUM_MISMATCH:
        return "checksum_mismatch";
    case MEM_SERVICE_UB_SSD_GSVA_IO_VERSION_CONFLICT:
        return "version_conflict";
    case MEM_SERVICE_UB_SSD_GSVA_IO_INTERNAL:
    default:
        return "internal";
    }
}

static int mem_service_qwen3_kv_publish_record_to_ub_ssd_gsva_backend(
    struct mem_service_cluster_runtime *rt,
    struct mem_service_record *record,
    uint64_t decode_step)
{
    struct mem_service_gsva_buffer_desc desc;
    struct mem_service_ub_ssd_gsva_io_request request;
    struct mem_service_ub_ssd_gsva_io_completion completion;
    enum mem_service_ub_ssd_gsva_io_status status;
    uint64_t key_hash;

    if (!rt || !record || !record->in_use) {
        return -1;
    }
    if (mem_service_cluster_runtime_make_gsva_buffer_desc(rt, record, &desc) != 0) {
        printf("[mem_service] stage qwen3_range_kv_state_ub_ssd_gsva_backend_attach"
               " key=%s step=%" PRIu64
               " status=not_attached reason=descriptor_unavailable\n",
               record->key,
               decode_step);
        return 0;
    }
    key_hash = mem_service_qwen3_kv_backend_key_hash(record->key);
    memset(&request, 0, sizeof(request));
    memset(&completion, 0, sizeof(completion));
    request.opcode = MEM_SERVICE_UB_SSD_GSVA_OP_BLOCK_WRITE;
    request.request_id = key_hash ^ (decode_step << 32) ^ record->version;
    request.source_cna = desc.source_cna;
    request.target_ssd_cna =
        mem_service_ub_ssd_gsva_device_cna_from_primary(desc.source_cna);
    request.block_ref.block_hi =
        ((uint64_t)record->kind << 32) | (uint64_t)record->object_owner_node;
    request.block_ref.block_lo = key_hash;
    request.block_ref.version = 0;
    request.block_ref.offset = 0;
    request.block_ref.bytes = record->object_backing_len;
    request.block_ref.checksum64 = record->object_payload_checksum;
    request.buffer = desc;
    status = mem_service_ub_ssd_gsva_submit(&request, &completion);
    if (status == MEM_SERVICE_UB_SSD_GSVA_IO_UNSUPPORTED) {
        printf("[mem_service] stage qwen3_range_kv_state_ub_ssd_gsva_backend_attach"
               " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64
               " status=not_attached reason=ub_ssd_device_unavailable\n",
               record->key,
               key_hash,
               decode_step);
        return 0;
    }
    if (status != MEM_SERVICE_UB_SSD_GSVA_IO_OK) {
        printf("[mem_service] gap qwen3_range_kv_state_ub_ssd_gsva_backend_attach=%s"
               " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64 "\n",
               mem_service_qwen3_kv_ub_ssd_status_name(status),
               record->key,
               key_hash,
               decode_step);
        return -1;
    }
    if (mem_service_record_attach_ub_ssd_gsva_backend_ref(record,
                                                          record->object_owner_node,
                                                          request.target_ssd_cna,
                                                          request.flags,
                                                          &completion.committed_ref,
                                                          false) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_ub_ssd_gsva_backend_attach"
           " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64
           " gsva_base=0x%016" PRIx64 " bytes=%" PRIu64
           " backend_device_cna=0x%08" PRIx32
           " backend_block_hi=%" PRIu64 " backend_block_lo=%" PRIu64
           " backend_block_version=%" PRIu64
           " backend_block_checksum=0x%016" PRIx64
           " primary_backing=obmm_shmem backend=ub_ssd_gsva status=ok\n",
           record->key,
           key_hash,
           decode_step,
           desc.gsva_base,
           desc.bytes,
           request.target_ssd_cna,
           completion.committed_ref.block_hi,
           completion.committed_ref.block_lo,
           completion.committed_ref.version,
           completion.committed_ref.checksum64);
    return 0;
}

static int mem_service_qwen3_kv_read_from_ub_ssd_gsva_backend(
    struct mem_service_cluster_runtime *rt,
    const struct mem_service_record *kv_state,
    uint32_t local_node,
    uint64_t kv_step,
    const uint8_t **payload_view_out,
    uint64_t *local_backing_offset_out)
{
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_read_buffer;
    struct mem_service_gsva_buffer_desc desc;
    struct mem_service_ub_ssd_gsva_io_request request;
    struct mem_service_ub_ssd_gsva_io_completion completion;
    enum mem_service_ub_ssd_gsva_io_status status;
    uint64_t local_offset = 0;
    uint64_t checksum;

    if (!rt || !kv_state || !payload_view_out || !local_backing_offset_out ||
        kv_state->object_backend_kind != MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA ||
        kv_state->object_backend_block_bytes != kv_state->object_backing_len ||
        kv_state->object_backend_block_checksum == 0 ||
        rt->local_idx < 0 || (uint32_t)rt->local_idx != local_node ||
        rt->local_idx >= rt->node_count) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                        kv_state->object_backing_len,
                                        64,
                                        &local_offset) != 0) {
        return -1;
    }
    memset(&local_read_buffer, 0, sizeof(local_read_buffer));
    local_read_buffer.in_use = true;
    local_read_buffer.kind = kv_state->kind;
    snprintf(local_read_buffer.key,
             sizeof(local_read_buffer.key),
             "%s",
             kv_state->key);
    local_read_buffer.version = kv_state->version;
    local_read_buffer.object_owner_node = local_node;
    local_read_buffer.object_payload_kind = kv_state->object_payload_kind;
    local_read_buffer.object_backing_offset = local_offset;
    local_read_buffer.object_backing_len = kv_state->object_backing_len;
    local_read_buffer.object_payload_checksum = kv_state->object_payload_checksum;
    if (mem_service_cluster_runtime_make_gsva_buffer_desc(rt,
                                                          &local_read_buffer,
                                                          &desc) != 0) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    memset(&completion, 0, sizeof(completion));
    request.opcode = MEM_SERVICE_UB_SSD_GSVA_OP_BLOCK_READ;
    request.request_id =
        kv_state->version ^ (kv_step << 32) ^ kv_state->object_backend_block_lo;
    request.source_cna = desc.source_cna;
    request.target_ssd_cna = kv_state->object_backend_device_cna;
    request.flags = kv_state->object_backend_flags;
    request.block_ref.block_hi = kv_state->object_backend_block_hi;
    request.block_ref.block_lo = kv_state->object_backend_block_lo;
    request.block_ref.version = kv_state->object_backend_block_version;
    request.block_ref.offset = kv_state->object_backend_block_offset;
    request.block_ref.bytes = kv_state->object_backend_block_bytes;
    request.block_ref.checksum64 = kv_state->object_backend_block_checksum;
    request.buffer = desc;
    status = mem_service_ub_ssd_gsva_submit(&request, &completion);
    if (status != MEM_SERVICE_UB_SSD_GSVA_IO_OK) {
        printf("[mem_service] gap qwen3_range_kv_state_ub_ssd_gsva_read=%s"
               " local=node%u key=%s kv_step=%" PRIu64
               " backend_device_cna=0x%08" PRIx32
               " block_hi=%" PRIu64 " block_lo=%" PRIu64
               " version=%" PRIu64 " bytes=%" PRIu64 "\n",
               mem_service_qwen3_kv_ub_ssd_status_name(status),
               local_node + 1U,
               kv_state->key,
               kv_step,
               request.target_ssd_cna,
               kv_state->object_backend_block_hi,
               kv_state->object_backend_block_lo,
               kv_state->object_backend_block_version,
               kv_state->object_backend_block_bytes);
        return -1;
    }
    if (completion.committed_ref.bytes != kv_state->object_backing_len ||
        completion.committed_ref.checksum64 !=
            kv_state->object_backend_block_checksum ||
        mem_service_update_region_range_at(local_slot,
                                           local_offset,
                                           kv_state->object_backing_len,
                                           false) != 0) {
        return -1;
    }
    checksum = mem_service_qwen3_hidden_payload_checksum(
        (const uint8_t *)local_slot->region.addr + local_offset,
        kv_state->object_backing_len);
    if (checksum != kv_state->object_payload_checksum) {
        uint64_t primary_checksum = 0;
        const char *primary_status = "unavailable";

        if (kv_state->object_backing_offset + kv_state->object_backing_len <=
            local_slot->region.len) {
            primary_checksum = mem_service_qwen3_hidden_payload_checksum(
                (const uint8_t *)local_slot->region.addr +
                    kv_state->object_backing_offset,
                kv_state->object_backing_len);
            primary_status = primary_checksum == kv_state->object_payload_checksum ?
                "matches_expected" : "mismatch";
        }
        printf("[mem_service] gap qwen3_range_kv_state_ub_ssd_gsva_read=checksum_mismatch"
               " local=node%u key=%s kv_step=%" PRIu64
               " checksum=0x%016" PRIx64 " expected=0x%016" PRIx64
               " backend_expected=0x%016" PRIx64
               " local_offset=0x%016" PRIx64
               " primary_offset=0x%016" PRIx64
               " primary_checksum=0x%016" PRIx64
               " primary_status=%s\n",
               local_node + 1U,
               kv_state->key,
               kv_step,
               checksum,
               kv_state->object_payload_checksum,
               kv_state->object_backend_block_checksum,
               local_offset,
               kv_state->object_backing_offset,
               primary_checksum,
               primary_status);
        return -1;
    }
    *payload_view_out = (const uint8_t *)local_slot->region.addr + local_offset;
    *local_backing_offset_out = local_offset;
    printf("[mem_service] stage qwen3_range_kv_state_ub_ssd_gsva_read"
           " local=node%u key=%s kv_step=%" PRIu64
           " gsva_base=0x%016" PRIx64 " bytes=%" PRIu64
           " backend_device_cna=0x%08" PRIx32
           " checksum=0x%016" PRIx64 " local_offset=0x%016" PRIx64
           " status=ok\n",
           local_node + 1U,
           kv_state->key,
           kv_step,
           desc.gsva_base,
           desc.bytes,
           request.target_ssd_cna,
           checksum,
           local_offset);
    return 0;
}

int mem_service_obmm_service_v0_publish_runtime_range_kv_state(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint8_t *kv_payload,
    uint64_t kv_payload_len,
    uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    char local_kv_state_key[256];
    uint64_t kv_checksum;
    uint64_t kv_state_offset = 0;
    uint64_t kv_state_block_bytes = 0;
    uint64_t kv_state_block_count = 0;
    uint64_t kv_state_reserved_bytes = 0;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;
    struct lingqu_object_ref_wire kv_ref;

    if (!svc || !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    kv_checksum = mem_service_qwen3_hidden_payload_checksum(kv_payload, kv_payload_len);
    if (kv_checksum != expected_kv_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
               local_node + 1U,
               kv_checksum,
               expected_kv_checksum,
               kv_payload_len);
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        return -1;
    }
    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + kv_state_offset, kv_payload, kv_payload_len);
    if (mem_service_update_region_range_at(local_slot,
                                     kv_state_offset,
                                     kv_payload_len,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + kv_state_offset, kv_payload_len, MS_SYNC);
    mem_service_qwen3_format_kv_state_key(local_kv_state_key,
                                          sizeof(local_kv_state_key),
                                          local_node,
                                          &local_placement,
                                          decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_KVCACHE_OBJECT,
                                     local_kv_state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
                                     kv_state_offset,
                                     kv_payload_len,
                                     kv_checksum,
                                     &local_kv_state) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, local_kv_state_key);

        if (!published_record) {
            return -1;
        }
        published_record->last_result_segment = (uint64_t)producer_publish_ms;
        published_record->object_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        published_record->object_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        published_record->object_publish_supernode_offset_ms =
            producer_clock_offset_ms;
        if (mem_service_qwen3_kv_publish_record_to_ub_ssd_gsva_backend(
                rt,
                published_record,
                decode_step) != 0) {
            return -1;
        }
        local_kv_state = *published_record;
    }
    if (mem_service_write_cluster_payload(rt, svc, local_slot) != 0 ||
        mem_service_record_to_lingqu_object_ref(&local_kv_state, &kv_ref) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_publish local=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " slot_bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " producer_publish_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service status=ok\n",
           local_node + 1U,
           decode_step,
           local_kv_state_key,
           kv_ref.key_hash,
           kv_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_payload_len,
           kv_checksum,
           kv_state_offset,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES,
           kv_state_block_bytes,
           kv_state_block_count,
           kv_state_reserved_bytes,
           producer_publish_ms,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_try_resolve_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t kv_step,
    struct mem_service_object_payload_view *view_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_record kv_state;
    char kv_state_key[256];
    const uint8_t *payload_view;
    uint64_t checksum;
    uint64_t resolved_backing_offset = 0;
    const char *resolved_backing = "obmm_shmem";
    const char *resolved_target = "mapped_view";
    bool backend_selected = false;

    if (!view_out) {
        return -1;
    }
    memset(view_out, 0, sizeof(*view_out));
    if (!svc ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0 ||
        !mem_service_read_qwen3_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr) {
        return -1;
    }
    mem_service_qwen3_format_kv_state_key(kv_state_key,
                                          sizeof(kv_state_key),
                                          local_node,
                                          &local_placement,
                                          kv_step);
    memset(&kv_state, 0, sizeof(kv_state));
    {
        struct mem_service_record *local_record = mem_service_find_record(svc, kv_state_key);

        if (local_record) {
            kv_state = *local_record;
        }
    }
    backend_selected =
        kv_state.object_backend_kind == MEM_SERVICE_OBJECT_BACKEND_UB_SSD_GSVA;
    if (kv_state.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        kv_state.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE ||
        kv_state.object_backing_len == 0 ||
        (!backend_selected &&
         kv_state.object_backing_offset + kv_state.object_backing_len >
             local_slot->region.len)) {
        printf("[mem_service] stage qwen3_range_kv_state_resolve_missing local=node%u kv_step=%" PRIu64 " key=%s status=miss\n",
               local_node + 1U,
               kv_step,
               kv_state_key);
        return 1;
    }
    checksum = kv_state.object_payload_checksum;
    if (backend_selected) {
        if (mem_service_qwen3_kv_read_from_ub_ssd_gsva_backend(
                rt,
                &kv_state,
                local_node,
                kv_step,
                &payload_view,
                &resolved_backing_offset) != 0) {
            return -1;
        }
        resolved_backing = "ub_ssd_gsva";
        resolved_target = "local_backend_read_buffer";
    } else {
        payload_view = (const uint8_t *)local_slot->region.addr +
                       kv_state.object_backing_offset;
        resolved_backing_offset = kv_state.object_backing_offset;
    }
    view_out->data = payload_view;
    view_out->len = kv_state.object_backing_len;
    view_out->checksum = checksum;
    view_out->owner_node = local_node;
    view_out->payload_kind = kv_state.object_payload_kind;
    view_out->backing_offset = resolved_backing_offset;
    if (mem_service_record_to_lingqu_object_ref(&kv_state, &view_out->object_ref) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_resolve local=node%u kv_step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " validation=object_ref_metadata source=obmm_object_view backing=%s metadata=lingqu_object_service target=%s status=ok\n",
           local_node + 1U,
           kv_step,
           kv_state_key,
           view_out->object_ref.key_hash,
           view_out->object_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           kv_state.object_backing_len,
           checksum,
           resolved_backing_offset,
           resolved_backing,
           resolved_target);
    return 0;
}

int mem_service_obmm_service_v0_resolve_previous_range_kv_state_view(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out)
{
    int rc;

    if (!view_out) {
        return -1;
    }
    memset(view_out, 0, sizeof(*view_out));
    if (decode_step == 0) {
        return 0;
    }
    rc = mem_service_obmm_service_v0_try_resolve_range_kv_state_view(
        svc,
        local_node,
        cluster_node_count,
        decode_step - 1U,
        view_out);
    if (rc > 0) {
        printf("[mem_service] gap qwen3_range_kv_state_resolve=missing local=node%u step=%" PRIu64 " previous_step=%" PRIu64 "\n",
               local_node + 1U,
               decode_step,
               decode_step - 1U);
        return -1;
    }
    return rc;
}

int mem_service_obmm_service_v0_resolve_previous_range_kv_state(struct mem_service *svc,
                                                          uint32_t local_node,
                                                          uint32_t cluster_node_count,
                                                          uint64_t decode_step,
                                                          uint8_t *payload_out,
                                                          uint64_t payload_capacity,
                                                          uint64_t *payload_len_out,
                                                          uint64_t *checksum_out)
{
    struct mem_service_object_payload_view view;

    if (!payload_len_out || !checksum_out) {
        return -1;
    }
    *payload_len_out = 0;
    *checksum_out = 0;
    if (!payload_out) {
        return -1;
    }
    if (mem_service_obmm_service_v0_resolve_previous_range_kv_state_view(
            svc,
            local_node,
            cluster_node_count,
            decode_step,
            &view) != 0) {
        return -1;
    }
    if (!view.data || view.len == 0) {
        return 0;
    }
    if (view.len > payload_capacity) {
        printf("[mem_service] gap qwen3_range_kv_state_resolve=payload_too_large local=node%u step=%" PRIu64 " bytes=%" PRIu64 " capacity=%" PRIu64 "\n",
               local_node + 1U,
               decode_step,
               view.len,
               payload_capacity);
        return -1;
    }
    memcpy(payload_out, view.data, (size_t)view.len);
    *payload_len_out = view.len;
    *checksum_out = view.checksum;
    return 0;
}
