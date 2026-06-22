static void mem_service_reset_remote_slots_for_publish(struct mem_service_cluster_runtime *rt)
{
    (void)rt;
}

int mem_service_cluster_fetch_record(struct mem_service *svc,
                               const char *key,
                               struct mem_service_record *resolved_out)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    long deadline;
    int i;
    int rc = -1;

    if (!svc || !key || !resolved_out) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (mem_service_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=write_local_payload_failed\n");
        return -1;
    }
    mem_service_reset_remote_slots_for_publish(rt);

    deadline = obmm_now_ms() + MEM_SERVICE_CLUSTER_WAIT_MS;
    while (obmm_now_ms() < deadline) {
        for (i = 0; i < rt->node_count; ++i) {
            if (!rt->slots[i].region.addr) {
                if (i != rt->local_idx && mem_service_activate_remote_slot(rt, i) != 0) {
                    continue;
                }
                if (!rt->slots[i].region.addr) {
                    continue;
                }
            }
            if (mem_service_slot_find_record(&rt->slots[i], key, resolved_out)) {
                rc = 0;
                break;
            }
        }
        if (rc == 0) {
            break;
        }
        usleep(10000);
    }
    if (rc != 0) {
        printf("[mem_service] gap db_service_cluster_stage=key_not_found key=%s\n", key);
    }
    return rc;
}

int mem_service_obmm_service_v0_ensure_cluster_runtime(uint32_t local_node,
                                                 uint32_t cluster_node_count)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    uint32_t i;

    if (cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    if (mem_service_cluster_runtime_init(rt) != 0) {
        return -1;
    }
    if ((uint32_t)rt->local_idx != local_node ||
        (uint32_t)rt->node_count != cluster_node_count ||
        !rt->slots[rt->local_idx].region.addr) {
        return -1;
    }
    if (rt->lazy_remote_activation) {
        printf("[mem_service] gap db_service_cluster_stage=lazy_activation_forbidden local=node%u\n",
               local_node + 1U);
        return -1;
    }
    for (i = 0; i < cluster_node_count; ++i) {
        if (!rt->slots[i].region.addr ||
            (i != local_node &&
             (!rt->egress_queues[i] || !rt->ingress_queues[i]))) {
            printf("[mem_service] gap db_service_cluster_stage=peer_not_bootstrapped local=node%u peer=node%u\n",
                   local_node + 1U,
                   i + 1U);
            return -1;
        }
    }
    printf("[mem_service] stage obmm_cluster_runtime_bootstrap local=node%u nodes=%u backing=obmm_pool metadata=lingqu_object_service queue=obmm_spsc status=ok\n",
           local_node + 1U,
           cluster_node_count);
    return 0;
}

int mem_service_publish_observe_cluster(struct mem_service *svc,
                                  const struct mem_service_record *local_record,
                                  struct mem_service_cluster_summary *summary)
{
    struct mem_service_cluster_runtime *rt = &g_mem_service_cluster_runtime;
    struct mem_service_cluster_payload *peer_snapshots = NULL;
    struct mem_service_cluster_payload_compact_summary peer_compact[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct mem_service_cluster_payload_header seen_header;
    bool peer_ready[MEM_SERVICE_CLUSTER_MAX_NODES] = { false };
    uint16_t local_publish_seq;
    uint16_t observed_seq;
    int i;
    int rc = -1;

    if (summary) {
        memset(summary, 0, sizeof(*summary));
    }
    if (!svc || !local_record || !summary) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0) {
        goto out;
    }
    peer_snapshots = calloc(MEM_SERVICE_CLUSTER_MAX_NODES, sizeof(*peer_snapshots));
    if (!peer_snapshots) {
        goto out;
    }

    summary->active = true;
    summary->placement_coherent = true;
    summary->state_coherent = true;
    summary->prefix_state_ready = true;
    summary->prefix_view_ready = true;
    summary->node_count = (uint32_t)rt->node_count;
    summary->local_version = local_record->version;
    summary->peer_version_floor = local_record->version;
    summary->peer_result_floor = local_record->last_result_segment;
    summary->peer_prefix_version_floor = 0;
    summary->peer_prefix_result_floor = 0;
    summary->peer_record_count_floor = 0;
    summary->peer_prefix_count_floor = 0;
    summary->peer_block_count_floor = 0;
    summary->peer_group_count_floor = 0;

    if (mem_service_write_cluster_payload(svc, &rt->slots[rt->local_idx]) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=write_local_payload_failed\n");
        goto out;
    }
    mem_service_reset_remote_slots_for_publish(rt);
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=remote_slots_reset seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    if (!mem_service_try_read_stable_payload_region(&rt->slots[rt->local_idx],
                                              &peer_snapshots[rt->local_idx],
                                              NULL)) {
        printf("[mem_service] gap db_service_cluster_stage=read_local_payload_failed\n");
        goto out;
    }
    mem_service_build_compact_summary(peer_snapshots[rt->local_idx].records,
                                peer_snapshots[rt->local_idx].record_count,
                                &peer_compact[rt->local_idx]);
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=read_local_payload_ok seq=%u\n",
           rt->local_idx + 1,
           rt->publish_seq);
    peer_ready[rt->local_idx] = true;
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    if (mem_service_queue_barrier(rt, OBMM_DESC_MEM_SERVICE_READY,
                            rt->observe_epoch,
                            local_publish_seq) != 0) {
        printf("[mem_service] gap db_service_cluster_stage=payload_ready_timeout epoch=%u\n",
               rt->observe_epoch);
        goto out;
    }
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=ready_barrier_ok epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           local_publish_seq);

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t owner_publish_seq = local_publish_seq;

        if (i == rt->local_idx) {
            peer_ready[i] = true;
            continue;
        }
        if (mem_service_activate_remote_slot(rt, i) != 0) {
            printf("[mem_service] gap db_service_cluster_stage=activate_remote_failed owner=node%d reader=node%d\n",
                   i + 1,
                   rt->local_idx + 1);
            goto out;
        }
        printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_wait expect_seq=%u mem_id=%" PRIu64 " map_osync=%d addr=%p\n",
               i + 1,
               rt->local_idx + 1,
               owner_publish_seq,
               rt->slots[i].mem_id,
               rt->slots[i].map_osync ? 1 : 0,
               rt->slots[i].region.addr);
        memset(&seen_header, 0, sizeof(seen_header));
        if (!mem_service_try_read_stable_compact_summary_region(&rt->slots[i],
                                                          &peer_compact[i],
                                                          &seen_header) ||
            seen_header.publish_done_seq < owner_publish_seq) {
            memset(&seen_header, 0, sizeof(seen_header));
            if (mem_service_wait_compact_summary_region_at_least(&rt->slots[i],
                                                           owner_publish_seq,
                                                           MEM_SERVICE_CLUSTER_WAIT_MS,
                                                           &peer_compact[i],
                                                           &seen_header)) {
            } else {
                printf("[mem_service] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d expect_seq=%u seen_seq=%u seen_done=%u magic=0x%08x version=%u count=%u\n",
                       i + 1,
                       rt->local_idx + 1,
                       owner_publish_seq,
                       seen_header.publish_seq,
                       seen_header.publish_done_seq,
                       seen_header.magic,
                       seen_header.version,
                       seen_header.record_count);
                printf("[mem_service] gap db_service_cluster_stage=payload_not_ready owner=node%d reader=node%d\n",
                       i + 1,
                       rt->local_idx + 1);
                goto out;
            }
        }
        printf("[mem_service] stage db_service_cluster_debug owner=node%d reader=node%d step=remote_payload_read_ok seq=%u expect_seq=%u\n",
               i + 1,
               rt->local_idx + 1,
               seen_header.publish_done_seq,
               owner_publish_seq);
        fflush(stdout);
        peer_ready[i] = true;
    }

    observed_seq = (uint16_t)(rt->local_idx + 1);
    rt->observe_epoch += 1;
    if (rt->observe_epoch == 0) {
        rt->observe_epoch = 1;
    }
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=observe_announce_begin epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);
    (void)mem_service_queue_barrier(rt, OBMM_DESC_MEM_SERVICE_OBSERVED,
                              rt->observe_epoch, observed_seq);
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=observe_announce_done epoch=%u seq=%u\n",
           rt->local_idx + 1,
           rt->observe_epoch,
           observed_seq);
    fflush(stdout);

    for (i = 0; i < rt->node_count; ++i) {
        uint16_t r;
        struct mem_service_cluster_payload_compact_summary compact = peer_compact[i];

        if (!peer_ready[i]) {
            printf("[mem_service] gap db_service_cluster_stage=payload_not_ready owner=node%d\n",
                   i + 1);
            goto out;
        }
        if (compact.record_count == 0 || compact.record_count > MEM_SERVICE_CLUSTER_MAX_RECORDS) {
            printf("[mem_service] gap db_service_cluster_stage=compact_summary_invalid owner=node%d count=%u\n",
                   i + 1,
                   compact.record_count);
            goto out;
        }
        if (summary->peer_record_count_floor == 0 ||
            compact.record_count < summary->peer_record_count_floor) {
            summary->peer_record_count_floor = compact.record_count;
        }
        if (i != rt->local_idx) {
            summary->peers_observed += 1;
        }
        if (compact.block_version_floor != 0 &&
            compact.block_version_floor < summary->peer_version_floor) {
            summary->peer_version_floor = compact.block_version_floor;
        }
        if (compact.block_result_floor != 0 &&
            compact.block_result_floor < summary->peer_result_floor) {
            summary->peer_result_floor = compact.block_result_floor;
        }
        if (summary->peer_prefix_version_floor == 0 ||
            (compact.prefix_version_floor != 0 &&
             compact.prefix_version_floor < summary->peer_prefix_version_floor)) {
            summary->peer_prefix_version_floor = compact.prefix_version_floor;
        }
        if (summary->peer_prefix_result_floor == 0 ||
            (compact.prefix_result_floor != 0 &&
             compact.prefix_result_floor < summary->peer_prefix_result_floor)) {
            summary->peer_prefix_result_floor = compact.prefix_result_floor;
        }
        if ((compact.flags & MEM_SERVICE_COMPACT_PREFIX_STATE_READY) == 0) {
            summary->prefix_state_ready = false;
        }
        if ((compact.flags & MEM_SERVICE_COMPACT_PREFIX_VIEW_READY) == 0) {
            summary->prefix_view_ready = false;
        }
        if (summary->peer_prefix_count_floor == 0 ||
            compact.prefix_count < summary->peer_prefix_count_floor) {
            summary->peer_prefix_count_floor = compact.prefix_count;
        }
        if (summary->peer_block_count_floor == 0 ||
            compact.block_count < summary->peer_block_count_floor) {
            summary->peer_block_count_floor = compact.block_count;
        }
        if (summary->peer_group_count_floor == 0 ||
            compact.group_count < summary->peer_group_count_floor) {
            summary->peer_group_count_floor = compact.group_count;
        }
        printf("[mem_service] stage db_service_cluster_observe_compact owner=node%d records=%u prefixes=%u blocks=%u groups=%u weight_tiles=%u kvcache_objects=%u block_version_floor=%" PRIu64 " prefix_version_floor=%" PRIu64 "\n",
               i + 1,
               compact.record_count,
               compact.prefix_count,
               compact.block_count,
               compact.group_count,
               compact.weight_tile_count,
               compact.kvcache_object_count,
               compact.block_version_floor,
               compact.prefix_version_floor);
        if (i != rt->local_idx) {
            continue;
        }
        for (r = 0; r < peer_snapshots[i].record_count; ++r) {
            struct mem_service_record *rec = &peer_snapshots[i].records[r];

            if (!rec->in_use) {
                goto out;
            }
            if (rec->kind == MEM_SERVICE_RECORD_REQUEST_PREFIX) {
                printf("[mem_service] stage db_service_cluster_observe owner=node%d kind=request_prefix key=%s version=%" PRIu64 "\n",
                       i + 1,
                       rec->key,
                       rec->version);
            } else if (rec->kind == MEM_SERVICE_RECORD_PREFIX_GROUP) {
                printf("[mem_service] stage db_service_cluster_observe owner=node%d kind=prefix_group key=%s group=%s members=%u state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       rec->group_id,
                       rec->member_count,
                       mem_service_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == MEM_SERVICE_RECORD_BLOCK_META) {
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    (rec->placement_node != local_record->placement_node ||
                     rec->placement_level != local_record->placement_level ||
                     rec->hot_segment_id != local_record->hot_segment_id)) {
                    summary->placement_coherent = false;
                }
                if (strncmp(rec->key, local_record->key, sizeof(rec->key)) == 0 &&
                    rec->state != local_record->state) {
                    summary->state_coherent = false;
                }
                printf("[mem_service] stage db_service_cluster_observe owner=node%d kind=block_meta key=%s state=%s version=%" PRIu64 " last_result_segment=0x%016" PRIx64 "\n",
                       i + 1,
                       rec->key,
                       mem_service_kvcache_state_name(rec->state),
                       rec->version,
                       rec->last_result_segment);
            } else if (rec->kind == MEM_SERVICE_RECORD_WEIGHT_TILE ||
                       rec->kind == MEM_SERVICE_RECORD_KVCACHE_OBJECT ||
                       rec->kind == MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT ||
                       rec->kind == MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT ||
                       rec->kind == MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                       rec->kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY ||
                       rec->kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                       rec->kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED ||
                       rec->kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE) {
                printf("[mem_service] stage db_service_cluster_observe owner=node%d kind=%s key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " version=%" PRIu64 "\n",
                       i + 1,
                       mem_service_object_kind_name(rec->object_payload_kind),
                       rec->key,
                       rec->object_backing_offset,
                       rec->object_backing_len,
                       rec->object_payload_checksum,
                       rec->version);
            } else if (rec->kind == MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT) {
                printf("[mem_service] stage db_service_cluster_observe owner=node%d kind=layer_range_placement key=%s owner_node=node%u layers=[%" PRIu64 ",%" PRIu64 ") count=%" PRIu64 " next=node%u checksum=0x%016" PRIx64 " version=%" PRIu64 "\n",
                       i + 1,
                       rec->key,
                       rec->placement_node + 1U,
                       rec->hot_segment_id,
                       rec->last_result_segment,
                       rec->object_backing_len,
                       rec->object_owner_node + 1U,
                       rec->object_payload_checksum,
                       rec->version);
            } else {
                goto out;
            }
        }
    }

    summary->ready = (summary->peers_observed == (uint32_t)(rt->node_count - 1));
    if (summary->ready) {
        printf("[mem_service] stage db_service_cluster=metadata_visible nodes=%u peers=%u local_version=%" PRIu64 " peer_version_floor=%" PRIu64 " peer_prefix_version_floor=%" PRIu64 " peer_prefix_result_floor=0x%016" PRIx64 " peer_record_count_floor=%u peer_prefix_count_floor=%u peer_block_count_floor=%u peer_group_count_floor=%u prefix_state_ready=%s prefix_view_ready=%s\n",
               summary->node_count,
               summary->peers_observed,
               summary->local_version,
               summary->peer_version_floor,
               summary->peer_prefix_version_floor,
               summary->peer_prefix_result_floor,
               summary->peer_record_count_floor,
               summary->peer_prefix_count_floor,
               summary->peer_block_count_floor,
               summary->peer_group_count_floor,
               summary->prefix_state_ready ? "true" : "false",
               summary->prefix_view_ready ? "true" : "false");
    }
    rc = 0;

out:
    free(peer_snapshots);
    return rc;
}
