#include "mem_service_internal.h"

#include "mem_service_obmm_object_flow.h"
#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"

int mem_service_obmm_service_v0_publish_resolve(struct mem_service *svc,
                                                uint32_t local_node,
                                                uint32_t cluster_node_count)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_record local_weight;
    struct mem_service_record local_kvcache;
    struct mem_service_record local_hidden_input;
    struct mem_service_record local_hidden_output;
    struct mem_service_record remote_weight;
    struct mem_service_record remote_kvcache;
    struct mem_service_record remote_hidden_input;
    struct mem_service_record remote_hidden_output;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_cluster_slot *remote_slot;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long deadline;
    uint8_t *base;
    uint64_t weight_checksum;
    uint64_t kvcache_checksum;
    uint64_t hidden_input_checksum;
    uint64_t hidden_output_checksum;
    uint64_t remote_weight_checksum;
    uint64_t remote_kvcache_checksum;
    uint64_t remote_hidden_input_checksum;
    uint64_t remote_hidden_output_checksum;
    bool remote_payload_checksums_match = false;
    char local_weight_key[96];
    char local_kvcache_key[96];
    char local_hidden_input_key[96];
    char local_hidden_output_key[96];
    char remote_weight_key[96];
    char remote_kvcache_key[96];
    char remote_hidden_input_key[96];
    char remote_hidden_output_key[96];
    uint16_t last_seen_seq = 0;
    uint16_t last_seen_done_seq = 0;
    uint16_t last_seen_record_count = 0;
    bool saw_remote_snapshot = false;
    bool got_remote_weight = false;
    bool got_remote_kvcache = false;
    bool got_remote_hidden_input = false;
    bool got_remote_hidden_output = false;
    unsigned int relax_attempt = 0;
    uint32_t local_range_start = 0;
    uint32_t local_range_end = 0;
    uint32_t remote_range_start = 0;
    uint32_t remote_range_end = 0;
    uint32_t prev_node;
    uint32_t remote_node;
    uint32_t hidden_input_seed_owner;
    uint32_t hidden_input_seed_kind;
    uint32_t total_layers;
    uint32_t min_layers;
    uint32_t max_layers;
    uint64_t hidden_range_bytes;
    uint64_t local_hidden_input_offset;
    uint64_t local_hidden_output_offset;
    struct mem_service_layer_range_placement local_placement;
    struct mem_service_layer_range_placement remote_placement;
    struct mem_service_layer_range_placement predecessor_placement;
    const char *model_key;

    memset(&local_placement, 0, sizeof(local_placement));
    memset(&remote_placement, 0, sizeof(remote_placement));
    memset(&predecessor_placement, 0, sizeof(predecessor_placement));
    total_layers = mem_service_model_layer_count();
    min_layers = 0;
    max_layers = 0;
    if (cluster_node_count != 0) {
        min_layers = total_layers / cluster_node_count;
        max_layers = min_layers + (total_layers % cluster_node_count ? 1U : 0U);
    }
    hidden_range_bytes = mem_service_model_hidden_range_bytes();
    local_hidden_input_offset = 0;
    local_hidden_output_offset = 0;
    model_key = mem_service_model_key();

    if (!svc || cluster_node_count == 0 || local_node >= cluster_node_count) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if ((uint32_t)rt->local_idx != local_node) {
        printf("[mem_service] gap obmm_service_v0=local_node_mismatch expected=%u actual=%d\n",
               local_node + 1U,
               rt->local_idx + 1);
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr ||
        local_slot->region.len <
            MEM_SERVICE_OBMM_HIDDEN_RANGE_RUNTIME_OUTPUT_OFFSET + hidden_range_bytes) {
        printf("[mem_service] gap obmm_service_v0=local_region_too_small len=%zu\n",
               local_slot->region.len);
        return -1;
    }
    if (cluster_node_count != mem_service_model_range_nodes()) {
        printf("[mem_service] gap qwen3_range_forward=node_count_mismatch nodes=%u expected=%u\n",
               cluster_node_count,
               mem_service_model_range_nodes());
        return -1;
    }
    if (mem_service_model_publish_layer_range_placements(svc,
                                                   cluster_node_count) != 0 ||
        !mem_service_model_read_layer_range_placement(svc,
                                                local_node,
                                                &local_placement)) {
        printf("[mem_service] gap qwen3_range_forward=placement_metadata_missing local=node%u nodes=%u\n",
               local_node + 1U,
               cluster_node_count);
        return -1;
    }
    remote_node = local_placement.next_owner_node;
    if (remote_node >= cluster_node_count || remote_node == local_node ||
        !mem_service_model_read_layer_range_placement(svc,
                                                remote_node,
                                                &remote_placement)) {
        printf("[mem_service] gap qwen3_range_forward=next_placement_metadata_missing local=node%u next=node%u nodes=%u\n",
               local_node + 1U,
               remote_node + 1U,
               cluster_node_count);
        return -1;
    }
    if (local_placement.layer_start == 0) {
        prev_node = local_node;
        hidden_input_seed_owner = local_node;
        hidden_input_seed_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT;
    } else if (mem_service_model_find_layer_range_predecessor(svc,
                                                        local_node,
                                                        &predecessor_placement) &&
               predecessor_placement.layer_end == local_placement.layer_start) {
        prev_node = predecessor_placement.owner_node;
        hidden_input_seed_owner = prev_node;
        hidden_input_seed_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT;
    } else {
        printf("[mem_service] gap qwen3_range_forward=predecessor_placement_missing local=node%u layers=[%u,%u)\n",
               local_node + 1U,
               local_placement.layer_start,
               local_placement.layer_end);
        return -1;
    }
    local_range_start = local_placement.layer_start;
    local_range_end = local_placement.layer_end;
    remote_range_start = remote_placement.layer_start;
    remote_range_end = remote_placement.layer_end;

    snprintf(local_weight_key,
             sizeof(local_weight_key),
             "weights/%s/node%u/tile0",
             model_key,
             local_node + 1U);
    snprintf(local_kvcache_key,
             sizeof(local_kvcache_key),
             "kvcache/mem_service/node%u/block0",
             local_node + 1U);
    snprintf(local_hidden_input_key,
             sizeof(local_hidden_input_key),
             "hidden/%s/node%u/range-input",
             model_key,
             local_node + 1U);
    snprintf(local_hidden_output_key,
             sizeof(local_hidden_output_key),
             "hidden/%s/node%u/range-output",
             model_key,
             local_node + 1U);
    snprintf(remote_weight_key,
             sizeof(remote_weight_key),
             "weights/%s/node%u/tile0",
             model_key,
             remote_node + 1U);
    snprintf(remote_kvcache_key,
             sizeof(remote_kvcache_key),
             "kvcache/mem_service/node%u/block0",
             remote_node + 1U);
    snprintf(remote_hidden_input_key,
             sizeof(remote_hidden_input_key),
             "hidden/%s/node%u/range-input",
             model_key,
             remote_node + 1U);
    snprintf(remote_hidden_output_key,
             sizeof(remote_hidden_output_key),
             "hidden/%s/node%u/range-output",
             model_key,
             remote_node + 1U);

    base = (uint8_t *)local_slot->region.addr;
    if (mem_service_payload_arena_alloc(rt,
                                  hidden_range_bytes,
                                  64,
                                  &local_hidden_input_offset) != 0 ||
        mem_service_payload_arena_alloc(rt,
                                  hidden_range_bytes,
                                  64,
                                  &local_hidden_output_offset) != 0) {
        printf("[mem_service] gap obmm_service_v0=hidden_range_arena_alloc_failed local=node%u bytes=%" PRIu64 "\n",
               local_node + 1U,
               hidden_range_bytes);
        return -1;
    }
    mem_service_fill_obmm_object_payload(base + MEM_SERVICE_OBMM_WEIGHT_OFFSET,
                                   MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                   local_node,
                                   MEM_SERVICE_OBMM_KIND_WEIGHT_TILE);
    mem_service_fill_obmm_object_payload(base + MEM_SERVICE_OBMM_KVCACHE_OFFSET,
                                   MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                   local_node,
                                   MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK);
    mem_service_fill_obmm_object_payload(base + local_hidden_input_offset,
                                   hidden_range_bytes,
                                   hidden_input_seed_owner,
                                   hidden_input_seed_kind);
    mem_service_fill_obmm_object_payload(base + local_hidden_output_offset,
                                   hidden_range_bytes,
                                   local_node,
                                   MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT);
    weight_checksum = mem_service_checksum_bytes(base + MEM_SERVICE_OBMM_WEIGHT_OFFSET,
                                           MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES);
    kvcache_checksum = mem_service_checksum_bytes(base + MEM_SERVICE_OBMM_KVCACHE_OFFSET,
                                            MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES);
    hidden_input_checksum =
        mem_service_checksum_bytes(base + local_hidden_input_offset,
                             hidden_range_bytes);
    hidden_output_checksum =
        mem_service_checksum_bytes(base + local_hidden_output_offset,
                             hidden_range_bytes);
    if (mem_service_update_region_range_at(local_slot,
                                     MEM_SERVICE_OBMM_WEIGHT_OFFSET,
                                     MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     MEM_SERVICE_OBMM_KVCACHE_OFFSET,
                                     MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     local_hidden_input_offset,
                                     hidden_range_bytes,
                                     true) != 0 ||
        mem_service_update_region_range_at(local_slot,
                                     local_hidden_output_offset,
                                     hidden_range_bytes,
                                     true) != 0) {
        printf("[mem_service] gap obmm_service_v0=local_payload_publish_failed\n");
        return -1;
    }
    (void)msync(base + MEM_SERVICE_OBMM_WEIGHT_OFFSET, MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES, MS_SYNC);
    (void)msync(base + MEM_SERVICE_OBMM_KVCACHE_OFFSET, MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES, MS_SYNC);
    (void)msync(base + local_hidden_input_offset,
                hidden_range_bytes,
                MS_SYNC);
    (void)msync(base + local_hidden_output_offset,
                hidden_range_bytes,
                MS_SYNC);

    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_WEIGHT_TILE,
                                     local_weight_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_WEIGHT_TILE,
                                     MEM_SERVICE_OBMM_WEIGHT_OFFSET,
                                     MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                     weight_checksum,
                                     &local_weight) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_KVCACHE_OBJECT,
                                     local_kvcache_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK,
                                     MEM_SERVICE_OBMM_KVCACHE_OFFSET,
                                     MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
                                     kvcache_checksum,
                                     &local_kvcache) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                                     local_hidden_input_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT,
                                     local_hidden_input_offset,
                                     hidden_range_bytes,
                                     hidden_input_checksum,
                                     &local_hidden_input) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output_offset,
                                     hidden_range_bytes,
                                     hidden_output_checksum,
                                     &local_hidden_output) != 0) {
        printf("[mem_service] gap obmm_service_v0=metadata_put_failed\n");
        return -1;
    }
    printf("[mem_service] stage obmm_service_v0_publish kind=weight_tile key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_weight.key,
           local_node + 1U,
           local_weight.object_backing_offset,
           local_weight.object_backing_len,
           local_weight.object_payload_checksum);
    printf("[mem_service] stage obmm_service_v0_publish kind=kvcache_block key=%s owner=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_kvcache.key,
           local_node + 1U,
           local_kvcache.object_backing_offset,
           local_kvcache.object_backing_len,
           local_kvcache.object_payload_checksum);
    printf("[mem_service] stage qwen3_range_forward_placement local=node%u key=placement/%s/layer-range/node%u layers=[%u,%u) count=%u next=node%u predecessor=node%u terminal=%s source=db_metadata strategy=%s status=ok\n",
           local_node + 1U,
           model_key,
           local_node + 1U,
           local_placement.layer_start,
           local_placement.layer_end,
           local_placement.layer_count,
           remote_node + 1U,
           prev_node + 1U,
           local_placement.terminal ? "true" : "false",
           "balanced_layers");
    printf("[mem_service] stage obmm_service_v0_publish kind=hidden_range_input key=%s owner=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_hidden_input.key,
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           local_hidden_input.object_backing_offset,
           local_hidden_input.object_backing_len,
           local_hidden_input.object_payload_checksum);
    printf("[mem_service] stage obmm_service_v0_publish kind=hidden_range_output key=%s owner=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           local_hidden_output.key,
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           local_hidden_output.object_backing_offset,
           local_hidden_output.object_backing_len,
           local_hidden_output.object_payload_checksum);
    printf("[mem_service] stage qwen3_range_forward_contract local=node%u layers=[%u,%u) count=%u next=node%u pipeline_nodes=%u total_layers=%u min_layers=%u max_layers=%u balanced=true placement_source=db_metadata input_key=%s output_key=%s kv_state_bytes_per_token=%" PRIu64 " backing=obmm_pool metadata=db status=ok\n",
           local_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_node + 1U,
           cluster_node_count,
           total_layers,
           min_layers,
           max_layers,
           local_hidden_input.key,
           local_hidden_output.key,
           mem_service_model_range_kv_state_bytes(local_range_start, local_range_end, 1));

    if (mem_service_write_cluster_payload(rt, svc, local_slot) != 0) {
        printf("[mem_service] gap obmm_service_v0=metadata_publish_failed\n");
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    object_epoch = rt->observe_epoch;
    (void)mem_service_queue_barrier(rt, OBMM_DESC_MEM_SERVICE_READY,
                              object_epoch, local_publish_seq);
    printf("[mem_service] stage obmm_service_v0_local_ready_announced local=node%u epoch=%u seq=%u\n",
           local_node + 1U,
           object_epoch,
           local_publish_seq);
    if (mem_service_push_obmm_object_descs(rt,
                                     MEM_SERVICE_OBMM_KIND_WEIGHT_TILE,
                                     local_weight.object_backing_offset,
                                     local_weight.object_backing_len,
                                     local_weight.object_payload_checksum,
                                     object_epoch) != 0 ||
        mem_service_push_obmm_object_descs(rt,
                                     MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK,
                                     local_kvcache.object_backing_offset,
                                     local_kvcache.object_backing_len,
                                     local_kvcache.object_payload_checksum,
                                     object_epoch) != 0 ||
        mem_service_push_obmm_object_descs(rt,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT,
                                     local_hidden_input.object_backing_offset,
                                     local_hidden_input.object_backing_len,
                                     local_hidden_input.object_payload_checksum,
                                     object_epoch) != 0 ||
        mem_service_push_obmm_object_descs(rt,
                                     MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
                                     local_hidden_output.object_backing_offset,
                                     local_hidden_output.object_backing_len,
                                     local_hidden_output.object_payload_checksum,
                                     object_epoch) != 0) {
        printf("[mem_service] gap obmm_service_v0=object_desc_publish_failed local=node%u epoch=%u\n",
               local_node + 1U,
               object_epoch);
        return -1;
    }
    printf("[mem_service] stage obmm_service_v0_object_desc_put local=node%u objects=4 queue=obmm_spsc epoch=%u status=ok\n",
           local_node + 1U,
           object_epoch);
    if (mem_service_activate_remote_slot(rt, (int)remote_node) != 0) {
        printf("[mem_service] gap obmm_service_v0=remote_slot_import_failed remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    remote_slot = &rt->slots[remote_node];
    deadline = obmm_now_ms() + MEM_SERVICE_OBMM_SERVICE_WAIT_MS;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_payload_header seen;

        memset(&seen, 0, sizeof(seen));
        if (mem_service_try_read_stable_compact_summary_region(remote_slot,
                                                         &(struct mem_service_cluster_payload_compact_summary){ 0 },
                                                         &seen)) {
            last_seen_seq = seen.publish_seq;
            last_seen_done_seq = seen.publish_done_seq;
            last_seen_record_count = seen.record_count;
            saw_remote_snapshot = true;
            got_remote_weight = mem_service_slot_find_record(remote_slot,
                                                       remote_weight_key,
                                                       &remote_weight);
            got_remote_kvcache = mem_service_slot_find_record(remote_slot,
                                                        remote_kvcache_key,
                                                        &remote_kvcache);
            got_remote_hidden_input = mem_service_slot_find_record(remote_slot,
                                                             remote_hidden_input_key,
                                                             &remote_hidden_input);
            got_remote_hidden_output = mem_service_slot_find_record(remote_slot,
                                                              remote_hidden_output_key,
                                                              &remote_hidden_output);
        }
        if (got_remote_weight && got_remote_kvcache &&
            got_remote_hidden_input && got_remote_hidden_output) {
            break;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    if (remote_weight.kind != MEM_SERVICE_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        remote_hidden_input.kind != MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT ||
        remote_hidden_output.kind != MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT) {
        printf("[mem_service] gap obmm_service_v0=remote_metadata_resolve_failed remote=node%u snapshot=%u seq=%u done=%u count=%u weight=%u kvcache=%u hidden_input=%u hidden_output=%u\n",
               remote_node + 1U,
               saw_remote_snapshot ? 1U : 0U,
               last_seen_seq,
               last_seen_done_seq,
               last_seen_record_count,
               got_remote_weight ? 1U : 0U,
               got_remote_kvcache ? 1U : 0U,
               got_remote_hidden_input ? 1U : 0U,
               got_remote_hidden_output ? 1U : 0U);
        return -1;
    }
    if (remote_weight.kind != MEM_SERVICE_RECORD_WEIGHT_TILE ||
        remote_kvcache.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        remote_hidden_input.kind != MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT ||
        remote_hidden_output.kind != MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT ||
        remote_weight.object_backing_len != MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES ||
        remote_kvcache.object_backing_len != MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES ||
        remote_hidden_input.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_len != hidden_range_bytes) {
        printf("[mem_service] gap obmm_service_v0=remote_metadata_incoherent remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    if (mem_service_wait_remote_obmm_object_descs(rt,
                                           remote_node,
                                           object_epoch,
                                           &remote_weight,
                                           &remote_kvcache,
                                           &remote_hidden_input,
                                           &remote_hidden_output) != 0) {
        return -1;
    }
    printf("[mem_service] stage obmm_service_v0_object_desc_get remote=node%u reader=node%u objects=4 queue=obmm_spsc epoch=%u status=ok\n",
           remote_node + 1U,
           local_node + 1U,
           object_epoch);
    if (!remote_slot->region.addr ||
        remote_weight.object_backing_offset + remote_weight.object_backing_len > remote_slot->region.len ||
        remote_kvcache.object_backing_offset + remote_kvcache.object_backing_len > remote_slot->region.len ||
        remote_hidden_input.object_backing_offset + remote_hidden_input.object_backing_len > remote_slot->region.len ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len > remote_slot->region.len) {
        printf("[mem_service] gap obmm_service_v0=remote_region_too_small remote=node%u\n",
               remote_node + 1U);
        return -1;
    }
    deadline = obmm_now_ms() + MEM_SERVICE_OBMM_SERVICE_WAIT_MS;
    do {
        remote_weight_checksum =
            mem_service_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_weight.object_backing_offset,
                                 remote_weight.object_backing_len);
        remote_kvcache_checksum =
            mem_service_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_kvcache.object_backing_offset,
                                 remote_kvcache.object_backing_len);
        remote_hidden_input_checksum =
            mem_service_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_hidden_input.object_backing_offset,
                                 remote_hidden_input.object_backing_len);
        remote_hidden_output_checksum =
            mem_service_checksum_bytes((const uint8_t *)remote_slot->region.addr +
                                     remote_hidden_output.object_backing_offset,
                                 remote_hidden_output.object_backing_len);
        remote_payload_checksums_match =
            remote_weight_checksum == remote_weight.object_payload_checksum &&
            remote_kvcache_checksum == remote_kvcache.object_payload_checksum &&
            remote_hidden_input_checksum == remote_hidden_input.object_payload_checksum &&
            remote_hidden_output_checksum == remote_hidden_output.object_payload_checksum;
        if (remote_payload_checksums_match) {
            break;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    } while (obmm_now_ms() < deadline);
    if (!remote_payload_checksums_match) {
        printf("[mem_service] gap obmm_service_v0=remote_payload_checksum_mismatch remote=node%u weight=0x%016" PRIx64 "/0x%016" PRIx64 " kvcache=0x%016" PRIx64 "/0x%016" PRIx64 " hidden_input=0x%016" PRIx64 "/0x%016" PRIx64 " hidden_output=0x%016" PRIx64 "/0x%016" PRIx64 "\n",
               remote_node + 1U,
               remote_weight_checksum,
               remote_weight.object_payload_checksum,
               remote_kvcache_checksum,
               remote_kvcache.object_payload_checksum,
               remote_hidden_input_checksum,
               remote_hidden_input.object_payload_checksum,
               remote_hidden_output_checksum,
               remote_hidden_output.object_payload_checksum);
        return -1;
    }
    printf("[mem_service] stage obmm_service_v0_resolve kind=weight_tile key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_weight.key,
           remote_node + 1U,
           local_node + 1U,
           remote_weight.object_backing_offset,
           remote_weight.object_backing_len,
           remote_weight_checksum);
    printf("[mem_service] stage obmm_service_v0_resolve kind=kvcache_block key=%s owner=node%u reader=node%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_kvcache.key,
           remote_node + 1U,
           local_node + 1U,
           remote_kvcache.object_backing_offset,
           remote_kvcache.object_backing_len,
           remote_kvcache_checksum);
    printf("[mem_service] stage obmm_service_v0_resolve kind=hidden_range_input key=%s owner=node%u reader=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_hidden_input.key,
           remote_node + 1U,
           local_node + 1U,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           remote_hidden_input.object_backing_offset,
           remote_hidden_input.object_backing_len,
           remote_hidden_input_checksum);
    printf("[mem_service] stage obmm_service_v0_resolve kind=hidden_range_output key=%s owner=node%u reader=node%u layers=[%u,%u) count=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " backing=obmm_pool metadata=db status=ok\n",
           remote_hidden_output.key,
           remote_node + 1U,
           local_node + 1U,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           remote_hidden_output.object_backing_offset,
           remote_hidden_output.object_backing_len,
           remote_hidden_output_checksum);
    if (!local_placement.terminal &&
        remote_hidden_input_checksum != hidden_output_checksum) {
        printf("[mem_service] gap qwen3_range_forward=next_input_checksum_mismatch local=node%u next=node%u output=0x%016" PRIx64 " next_input=0x%016" PRIx64 "\n",
               local_node + 1U,
               remote_node + 1U,
               hidden_output_checksum,
               remote_hidden_input_checksum);
        return -1;
    }
    printf("[mem_service] stage qwen3_range_forward_handoff local=node%u next=node%u local_layers=[%u,%u) local_count=%u next_layers=[%u,%u) next_count=%u local_output_checksum=0x%016" PRIx64 " next_input_checksum=0x%016" PRIx64 " terminal=%s placement_source=db_metadata backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           remote_node + 1U,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_range_start,
           remote_range_end,
           remote_range_end - remote_range_start,
           hidden_output_checksum,
           remote_hidden_input_checksum,
           local_placement.terminal ? "true" : "false");
    printf("[mem_service] stage qwen3_range_forward_summary local=node%u nodes=%u layers=%u assigned_layers=[%u,%u) assigned_count=%u next=node%u hidden_bytes=%" PRIu64 " objects=2 min_layers=%u max_layers=%u balanced=true placement_source=db_metadata backing=obmm_pool metadata=db status=ok\n",
           local_node + 1U,
           cluster_node_count,
           total_layers,
           local_range_start,
           local_range_end,
           local_range_end - local_range_start,
           remote_node + 1U,
           hidden_range_bytes,
           min_layers,
           max_layers);
    printf("[mem_service] stage obmm_service_v0=payload_backing_resolved local=node%u remote=node%u objects=4 bytes=%" PRIu64 " hidden_bytes=%" PRIu64 " hidden_input_offset=0x%016" PRIx64 " hidden_output_offset=0x%016" PRIx64 " backing=obmm_pool allocator=linear_payload_arena metadata=db status=ok\n",
           local_node + 1U,
           remote_node + 1U,
           (uint64_t)MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES,
           hidden_range_bytes,
           local_hidden_input_offset,
           local_hidden_output_offset);
    return 0;
}
