int mem_service_obmm_service_v0_publish_runtime_range_kv_state(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    const uint8_t *kv_payload,
    uint64_t kv_payload_len,
    uint64_t expected_kv_checksum)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_kv_state;
    struct mem_service_qwen3_layer_range_placement local_placement;
    char local_kv_state_key[96];
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
    struct lingqu_obmm_object_ref_wire kv_ref;

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
    snprintf(local_kv_state_key,
             sizeof(local_kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
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
        local_kv_state.last_result_segment = (uint64_t)producer_publish_ms;
        local_kv_state.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_kv_state.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_kv_state.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(svc, local_slot) != 0 ||
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
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_record kv_state;
    char kv_state_key[96];
    uint64_t checksum;

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
    snprintf(kv_state_key,
             sizeof(kv_state_key),
             "kvcache/%s/node%u/layers-%u-%u/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             local_node + 1U,
             local_placement.layer_start,
             local_placement.layer_end,
             kv_step);
    memset(&kv_state, 0, sizeof(kv_state));
    {
        struct mem_service_record *local_record = mem_service_find_record(svc, kv_state_key);

        if (local_record) {
            kv_state = *local_record;
        }
    }
    if (kv_state.kind != MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
        kv_state.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE ||
        kv_state.object_backing_len == 0 ||
        kv_state.object_backing_offset + kv_state.object_backing_len >
            local_slot->region.len) {
        printf("[mem_service] stage qwen3_range_kv_state_resolve_missing local=node%u kv_step=%" PRIu64 " key=%s status=miss\n",
               local_node + 1U,
               kv_step,
               kv_state_key);
        return 1;
    }
    checksum = kv_state.object_payload_checksum;
    view_out->data = (const uint8_t *)local_slot->region.addr +
                     kv_state.object_backing_offset;
    view_out->len = kv_state.object_backing_len;
    view_out->checksum = checksum;
    view_out->owner_node = local_node;
    view_out->payload_kind = kv_state.object_payload_kind;
    view_out->backing_offset = kv_state.object_backing_offset;
    if (mem_service_record_to_lingqu_obmm_ref(&kv_state, &view_out->object_ref) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_range_kv_state_resolve local=node%u kv_step=%" PRIu64 " key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) count=%u kv_bytes=%" PRIu64 " kv_checksum=0x%016" PRIx64 " offset=0x%016" PRIx64 " validation=object_ref_metadata source=obmm_object_view backing=obmm_shmem metadata=lingqu_object_service target=mapped_view status=ok\n",
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
           kv_state.object_backing_offset);
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
