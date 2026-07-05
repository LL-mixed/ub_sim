#include "mem_service_internal.h"

#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"

static void mem_service_qwen3_format_token_result_key(
    char *key,
    size_t key_len,
    uint64_t decode_step)
{
    uint64_t run_scope_hash = mem_service_run_scope_hash_from_env();

    if (!key || key_len == 0) {
        return;
    }
    if (run_scope_hash != 0) {
        snprintf(key,
                 key_len,
                 "tokens/%s/scope/%016" PRIx64 "/decode-step%" PRIu64,
                 mem_service_qwen3_model_key(),
                 run_scope_hash,
                 decode_step);
        return;
    }
    snprintf(key,
             key_len,
             "tokens/%s/decode-step%" PRIu64,
             mem_service_qwen3_model_key(),
             decode_step);
}

static int mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1,
    bool require_terminal_node)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record local_token_result;
    struct mem_service_qwen3_layer_range_placement local_placement;
    char token_result_key[256];
    uint64_t payload_words[8];
    uint64_t token_result_offset;
    uint64_t checksum;
    uint32_t target_node;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
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
    if (require_terminal_node && !local_placement.terminal) {
        return 0;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                  64,
                                  &token_result_offset) != 0) {
        return -1;
    }

    payload_words[0] = decode_step;
    payload_words[1] = sampled_token;
    payload_words[2] = runner_up_token;
    payload_words[3] = margin_milli;
    payload_words[4] = logits_checksum;
    payload_words[5] = text_checksum;
    payload_words[6] = piece_word0;
    payload_words[7] = piece_word1;
    checksum = mem_service_qwen3_hidden_payload_checksum(
        (const uint8_t *)payload_words,
        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + token_result_offset, payload_words, MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
    if (mem_service_update_region_range_at(local_slot,
                                     token_result_offset,
                                     MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     true) != 0) {
        return -1;
    }
    (void)msync(base + token_result_offset, MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES, MS_SYNC);

    mem_service_qwen3_format_token_result_key(token_result_key,
                                              sizeof(token_result_key),
                                              decode_step);
    producer_publish_monotonic_ms = obmm_now_ms();
    producer_publish_ms = mem_service_wallclock_ms();
    producer_clock_offset_ms = producer_publish_ms - producer_publish_monotonic_ms;
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                                     token_result_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                                     token_result_offset,
                                     MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                                     checksum,
                                     &local_token_result) != 0) {
        return -1;
    }
    {
        struct mem_service_record *published_record =
            mem_service_find_record(svc, token_result_key);

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
        local_token_result.last_result_segment = (uint64_t)producer_publish_ms;
        local_token_result.object_publish_monotonic_ms =
            published_record->object_publish_monotonic_ms;
        local_token_result.object_publish_supernode_ms =
            published_record->object_publish_supernode_ms;
        local_token_result.object_publish_supernode_offset_ms =
            published_record->object_publish_supernode_offset_ms;
    }
    if (mem_service_write_cluster_payload(rt, svc, local_slot) != 0) {
        return -1;
    }
    local_publish_seq = (uint16_t)(rt->publish_seq & 0xffffu);
    if (local_publish_seq == 0) {
        local_publish_seq = 1;
    }
    if (mem_service_qwen3_decode_entry_node(cluster_node_count, &target_node) != 0) {
        return -1;
    }
    object_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (object_epoch == 0) {
        object_epoch = 1;
    }
    if (!require_terminal_node) {
        uint32_t node_idx;

        for (node_idx = 0; node_idx < cluster_node_count; ++node_idx) {
            if (node_idx == (uint32_t)rt->local_idx) {
                struct obmm_desc desc;

                if (rt->pending_desc_count[rt->local_idx] >=
                    MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH) {
                    return -1;
                }
                memset(&desc, 0, sizeof(desc));
                desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
                desc.flags = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
                desc.seq = ((uint64_t)object_epoch << 48) |
                           ((uint64_t)(rt->local_idx + 1) << 32) |
                           (local_token_result.object_backing_offset &
                            0xffffffffULL);
                desc.region_id = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
                desc.payload_len =
                    (uint32_t)local_token_result.object_backing_len;
                desc.payload_offset = local_token_result.object_backing_offset;
                desc.cookie =
                    (uint32_t)(local_token_result.object_payload_checksum ^
                               (local_token_result.object_payload_checksum >>
                                32));
                mem_service_stash_pending_desc(rt, rt->local_idx, &desc);
            } else if (mem_service_push_obmm_object_desc_to(
                           rt,
                           node_idx,
                           MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                           local_token_result.object_backing_offset,
                           local_token_result.object_backing_len,
                           local_token_result.object_payload_checksum,
                           object_epoch) != 0) {
                return -1;
            }
        }
    } else if (target_node == (uint32_t)rt->local_idx) {
        struct obmm_desc desc;

        if (rt->pending_desc_count[rt->local_idx] >=
            MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH) {
            return -1;
        }
        memset(&desc, 0, sizeof(desc));
        desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
        desc.flags = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
        desc.seq = ((uint64_t)object_epoch << 48) |
                   ((uint64_t)(rt->local_idx + 1) << 32) |
                   (local_token_result.object_backing_offset & 0xffffffffULL);
        desc.region_id = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT;
        desc.payload_len = (uint32_t)local_token_result.object_backing_len;
        desc.payload_offset = local_token_result.object_backing_offset;
        desc.cookie =
            (uint32_t)(local_token_result.object_payload_checksum ^
                       (local_token_result.object_payload_checksum >> 32));
        mem_service_stash_pending_desc(rt, rt->local_idx, &desc);
    } else if (mem_service_push_obmm_object_desc_to(
                   rt,
                   target_node,
                   MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                   local_token_result.object_backing_offset,
                   local_token_result.object_backing_len,
                   local_token_result.object_payload_checksum,
                   object_epoch) != 0) {
        return -1;
    }
    printf("[mem_service] stage qwen3_terminal_token_result_publish local=node%u target=node%u step=%" PRIu64 " token=%" PRIu64 " runner_up=%" PRIu64 " margin_milli=%" PRIu64 " logits_checksum=0x%016" PRIx64 " text_checksum=0x%016" PRIx64 " piece_word0=0x%016" PRIx64 " piece_word1=0x%016" PRIx64 " object_key=%s offset=0x%016" PRIx64 " bytes=%" PRIu64 " checksum=0x%016" PRIx64 " epoch=%u seq=%u backing=obmm_pool metadata=db queue=%s status=ok publisher=%s broadcast_targets=%u\n",
           local_node + 1U,
           target_node + 1U,
           decode_step,
           sampled_token,
           runner_up_token,
           margin_milli,
           logits_checksum,
           text_checksum,
           piece_word0,
           piece_word1,
           token_result_key,
           local_token_result.object_backing_offset,
           local_token_result.object_backing_len,
           local_token_result.object_payload_checksum,
           object_epoch,
           local_publish_seq,
           target_node == (uint32_t)rt->local_idx ? "local_pending" : "obmm_spsc",
           require_terminal_node ? "terminal_node" : "shortpath_boundary",
           require_terminal_node ? 1U : cluster_node_count);
    return 0;
}

int mem_service_obmm_service_v0_publish_terminal_token_result(struct mem_service *svc,
                                                        uint32_t local_node,
                                                        uint32_t cluster_node_count,
                                                        uint64_t decode_step,
                                                        uint64_t sampled_token,
                                                        uint64_t runner_up_token,
                                                        uint64_t margin_milli,
                                                        uint64_t logits_checksum,
                                                        uint64_t text_checksum,
                                                        uint64_t piece_word0,
                                                        uint64_t piece_word1)
{
    return mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
        svc,
        local_node,
        cluster_node_count,
        decode_step,
        sampled_token,
        runner_up_token,
        margin_milli,
        logits_checksum,
        text_checksum,
        piece_word0,
        piece_word1,
        true);
}

int mem_service_obmm_service_v0_publish_shortpath_terminal_token_result(
    struct mem_service *svc,
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    uint64_t sampled_token,
    uint64_t runner_up_token,
    uint64_t margin_milli,
    uint64_t logits_checksum,
    uint64_t text_checksum,
    uint64_t piece_word0,
    uint64_t piece_word1)
{
    return mem_service_obmm_service_v0_publish_terminal_token_result_from_node(
        svc,
        local_node,
        cluster_node_count,
        decode_step,
        sampled_token,
        runner_up_token,
        margin_milli,
        logits_checksum,
        text_checksum,
        piece_word0,
        piece_word1,
        false);
}

int mem_service_obmm_service_v0_wait_terminal_token_result(struct mem_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *sampled_token_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    char token_result_key[256];
    long deadline;
    bool first_scan = true;

    if (!svc) {
        return -1;
    }
    mem_service_qwen3_format_token_result_key(token_result_key,
                                              sizeof(token_result_key),
                                              decode_step);
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (first_scan || obmm_now_ms() < deadline) {
        first_scan = false;
        if (mem_service_cluster_runtime_require(rt) == 0) {
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                struct mem_service_record token_record;
                uint64_t payload_words[8];
                uint64_t checksum;
                struct mem_service_cluster_slot *owner_slot;

                if (owner_idx != rt->local_idx &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                owner_slot = &rt->slots[owner_idx];
                if (owner_slot->region.addr &&
                    mem_service_try_read_stable_compact_summary_region(owner_slot,
                                                                 &compact,
                                                                 &seen) &&
                    mem_service_slot_find_record(owner_slot,
                                           token_result_key,
                                           &token_record) &&
                    token_record.kind == MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT &&
                    token_record.object_payload_kind ==
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT &&
                    token_record.object_backing_len ==
                        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES &&
                    token_record.object_backing_offset <= owner_slot->region.len &&
                    token_record.object_backing_len <=
                        owner_slot->region.len - token_record.object_backing_offset) {
                    memcpy(payload_words,
                           (uint8_t *)owner_slot->region.addr +
                               token_record.object_backing_offset,
                           MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                    if (payload_words[0] == decode_step) {
                        checksum = mem_service_qwen3_hidden_payload_checksum(
                            (const uint8_t *)payload_words,
                            MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                        if (checksum != token_record.object_payload_checksum) {
                            usleep(10000);
                            continue;
                        }
                        if (sampled_token_out) {
                            *sampled_token_out = payload_words[1];
                        }
                        printf("[mem_service] stage qwen3_terminal_token_result_wait step=%" PRIu64
                               " object_key=%s owner=node%d offset=0x%016" PRIx64
                               " bytes=%" PRIu64
                               " token=%" PRIu64 " checksum=0x%016" PRIx64
                               " source=obmm_object_record status=ok\n",
                               decode_step,
                               token_result_key,
                               owner_idx + 1,
                               token_record.object_backing_offset,
                               (uint64_t)MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
                               payload_words[1],
                               checksum);
                        return 0;
                    }
                }
            }
        }
        if (timeout_ms == 0) {
            break;
        }
        usleep(10000);
    }
    if (timeout_ms != 0) {
        printf("[mem_service] gap qwen3_terminal_token_result_wait=timeout step=%" PRIu64
               " object_key=%s\n",
               decode_step,
               token_result_key);
    }
    return -1;
}
