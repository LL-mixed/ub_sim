#include "mem_service_internal.h"

#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"
#include "mem_service_ub_ssd_gsva_io.h"

static void mem_service_qwen3_format_runtime_range_key(
    char *key,
    size_t key_len,
    bool terminal_range,
    uint32_t target_node,
    uint64_t decode_step)
{
    uint64_t run_scope_hash = mem_service_run_scope_hash_from_env();
    uint64_t object_decode_step =
        mem_service_serving_decode_step_from_env(decode_step);

    if (!key || key_len == 0) {
        return;
    }
    if (run_scope_hash != 0) {
        snprintf(key,
                 key_len,
                 terminal_range ?
                     "hidden/%s/scope/%016" PRIx64 "/node%u/range-runtime-output/decode-step%" PRIu64 :
                     "hidden/%s/scope/%016" PRIx64 "/node%u/range-runtime-input/decode-step%" PRIu64,
                 mem_service_qwen3_model_key(),
                 run_scope_hash,
                 target_node + 1U,
                 object_decode_step);
        return;
    }
    snprintf(key,
             key_len,
             terminal_range ?
                 "hidden/%s/node%u/range-runtime-output/decode-step%" PRIu64 :
                 "hidden/%s/node%u/range-runtime-input/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             target_node + 1U,
             object_decode_step);
}

static void mem_service_qwen3_format_runtime_kv_key(
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

static uint64_t mem_service_qwen3_backend_key_hash(const char *key)
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

static const char *mem_service_qwen3_ub_ssd_status_name(
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

static int mem_service_qwen3_publish_record_to_ub_ssd_gsva_backend(
    struct mem_service_cluster_runtime *rt,
    struct mem_service_record *record,
    uint64_t decode_step,
    const char *stage_name)
{
    struct mem_service_ub_ssd_gsva_buffer_desc desc;
    struct mem_service_ub_ssd_gsva_io_request request;
    struct mem_service_ub_ssd_gsva_io_completion completion;
    enum mem_service_ub_ssd_gsva_io_status status;
    uint64_t key_hash;

    if (!rt || !record || !record->in_use || !stage_name) {
        return -1;
    }
    if (mem_service_cluster_runtime_make_ub_ssd_gsva_buffer_desc(rt,
                                                                  record,
                                                                  &desc) != 0) {
        printf("[mem_service] stage %s_ub_ssd_gsva_backend_attach"
               " key=%s step=%" PRIu64
               " status=not_attached reason=descriptor_unavailable\n",
               stage_name,
               record->key,
               decode_step);
        return 0;
    }
    key_hash = mem_service_qwen3_backend_key_hash(record->key);
    memset(&request, 0, sizeof(request));
    memset(&completion, 0, sizeof(completion));
    request.opcode = MEM_SERVICE_UB_SSD_GSVA_OP_BLOCK_WRITE;
    request.request_id = key_hash ^ (decode_step << 32) ^ record->version;
    request.source_cna = desc.source_cna;
    request.target_ssd_cna = record->object_backend_device_cna;
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
        printf("[mem_service] stage %s_ub_ssd_gsva_backend_attach"
               " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64
               " status=not_attached reason=ub_ssd_device_unavailable\n",
               stage_name,
               record->key,
               key_hash,
               decode_step);
        return 0;
    }
    if (status != MEM_SERVICE_UB_SSD_GSVA_IO_OK) {
        printf("[mem_service] gap %s_ub_ssd_gsva_backend_attach=%s"
               " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64 "\n",
               stage_name,
               mem_service_qwen3_ub_ssd_status_name(status),
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
    printf("[mem_service] stage %s_ub_ssd_gsva_backend_attach"
           " key=%s key_hash=0x%016" PRIx64 " step=%" PRIu64
           " gsva_base=0x%016" PRIx64 " bytes=%" PRIu64
           " backend_block_hi=%" PRIu64 " backend_block_lo=%" PRIu64
           " backend_block_version=%" PRIu64
           " backend_block_checksum=0x%016" PRIx64
           " primary_backing=obmm_shmem backend=ub_ssd_gsva status=ok\n",
           stage_name,
           record->key,
           key_hash,
           decode_step,
           desc.gsva_base,
           desc.bytes,
           completion.committed_ref.block_hi,
           completion.committed_ref.block_lo,
           completion.committed_ref.version,
           completion.committed_ref.checksum64);
    return 0;
}

int mem_service_obmm_service_v0_publish_runtime_range_output(struct mem_service *svc,
                                                       uint32_t local_node,
                                                       uint32_t cluster_node_count,
                                                       uint64_t decode_step,
                                                       const uint8_t *payload,
                                                       uint64_t payload_len,
                                                       uint64_t expected_checksum,
                                                       const uint8_t *kv_payload,
                                                       uint64_t kv_payload_len,
                                                       uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_hidden_output;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    uint32_t target_node;
    bool terminal_range;
    char local_hidden_output_key[256];
    char local_kv_state_key[256];
    uint64_t checksum;
    uint64_t kv_checksum = 0;
    uint64_t kv_state_offset = 0;
    uint64_t kv_state_block_bytes = 0;
    uint64_t kv_state_block_count = 0;
    uint64_t kv_state_reserved_bytes = 0;
    uint64_t runtime_output_offset = 0;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    struct lingqu_obmm_object_ref_wire hidden_ref;
    struct lingqu_obmm_object_ref_wire kv_ref;

    if (!svc || !payload || payload_len != hidden_range_bytes ||
        !kv_payload || kv_payload_len == 0 ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    checksum = mem_service_qwen3_hidden_payload_checksum(payload, payload_len);
    if (checksum != expected_checksum) {
        printf("[mem_service] gap qwen3_range_forward=runtime_output_checksum_mismatch local=node%u checksum=0x%016" PRIx64 " expected=0x%016" PRIx64 "\n",
               local_node + 1U,
               checksum,
               expected_checksum);
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
    terminal_range = local_placement.layer_end >= mem_service_qwen3_layer_count();
    target_node = terminal_range ? local_node : local_placement.next_owner_node;
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  payload_len,
                                  64,
                                  &runtime_output_offset) != 0) {
        return -1;
    }
    if (mem_service_qwen3_kv_state_alloc(rt,
                                   kv_payload_len,
                                   &kv_state_offset,
                                   &kv_state_block_bytes,
                                   &kv_state_block_count,
                                   &kv_state_reserved_bytes) != 0) {
        printf("[mem_service] gap qwen3_range_forward=runtime_kv_block_span_alloc_failed local=node%u step=%" PRIu64 " bytes=%" PRIu64 " block_bytes=%" PRIu64 " blocks=%" PRIu64 " reserved_bytes=%" PRIu64 " region_len=%zu\n",
               local_node + 1U,
               decode_step,
               kv_payload_len,
               kv_state_block_bytes,
               kv_state_block_count,
               kv_state_reserved_bytes,
               local_slot->region.len);
        return -1;
    }
    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + runtime_output_offset, payload, payload_len);
    memcpy(base + kv_state_offset, kv_payload, kv_payload_len);
    if (mem_service_update_region_range_at(local_slot,
                                     runtime_output_offset,
                                     payload_len,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     kv_state_offset,
                                     kv_payload_len,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + runtime_output_offset,
                payload_len,
                MS_SYNC);
    (void)msync(base + kv_state_offset, kv_payload_len, MS_SYNC);
    mem_service_qwen3_format_runtime_range_key(local_hidden_output_key,
                                               sizeof(local_hidden_output_key),
                                               terminal_range,
                                               target_node,
                                               decode_step);
    mem_service_qwen3_format_runtime_kv_key(local_kv_state_key,
                                            sizeof(local_kv_state_key),
                                            local_node,
                                            &local_placement,
                                            decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     terminal_range ?
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT :
                                         MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                     runtime_output_offset,
                                     payload_len,
                                     checksum,
                                     &local_hidden_output) != 0 ||
        mem_service_put_obmm_object_record(svc,
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
            mem_service_find_record(svc, local_hidden_output_key);

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
        if (mem_service_qwen3_publish_record_to_ub_ssd_gsva_backend(
                rt,
                published_record,
                decode_step,
                "qwen3_range_runtime_output") != 0) {
            return -1;
        }
        local_hidden_output = *published_record;
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
        if (mem_service_qwen3_publish_record_to_ub_ssd_gsva_backend(
                rt,
                published_record,
                decode_step,
                "qwen3_range_kv_state") != 0) {
            return -1;
        }
        local_kv_state = *published_record;
    }
    if (mem_service_write_cluster_payload(rt, svc, local_slot) != 0) {
        return -1;
    }
    if (mem_service_record_to_lingqu_obmm_ref(&local_hidden_output, &hidden_ref) != 0 ||
        mem_service_record_to_lingqu_obmm_ref(&local_kv_state, &kv_ref) != 0) {
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
    char boundary_observation_id[384];
    const char *service_run_id = mem_service_run_id_from_env();

    boundary_observation_id[0] = '\0';
    if (service_run_id) {
        snprintf(boundary_observation_id,
                 sizeof(boundary_observation_id),
                 "boundary-observation/%s/step%" PRIu64 "/node%u",
                 service_run_id,
                 decode_step,
                 local_node + 1U);
    }
    if (!terminal_range &&
        mem_service_push_obmm_object_desc_to(rt,
                                       target_node,
                                       MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                                       local_hidden_output.object_backing_offset,
                                       local_hidden_output.object_backing_len,
                                       local_hidden_output.object_payload_checksum,
                                       object_epoch) != 0) {
        return -1;
    }
    if (!terminal_range && boundary_observation_id[0] != '\0') {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u observation_id=%s step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               boundary_observation_id,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    } else if (!terminal_range) {
        printf("[mem_service] stage qwen3_range_forward_runtime_ingress_publish local=node%u target=node%u step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
               local_node + 1U,
               target_node + 1U,
               decode_step,
               local_hidden_output_key,
               hidden_ref.key_hash,
               hidden_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               local_placement.layer_count,
               checksum,
               payload_len,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               object_epoch,
               local_publish_seq);
    }
    printf("[mem_service] stage qwen3_range_forward_runtime_output_publish local=node%u step=%" PRIu64 " key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u output_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld epoch=%u seq=%u backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           hidden_ref.key_hash,
           hidden_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           checksum,
           payload_len,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           object_epoch,
           local_publish_seq);
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
