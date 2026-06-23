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

static uint64_t mem_service_pack_qwen3_engram_candidates(uint64_t decode_step,
                                                   const uint64_t *candidate_tokens,
                                                   const uint64_t *candidate_logit_bits,
                                                   const uint64_t *candidate_text_checksums,
                                                   const uint64_t *candidate_piece_bytes,
                                                   const uint64_t *candidate_piece_word0,
                                                   const uint64_t *candidate_piece_word1,
                                                   uint64_t candidate_count,
                                                   uint64_t *candidate_words,
                                                   size_t candidate_word_count)
{
    uint64_t packed_count;

    if (!candidate_words || candidate_word_count < 32U || !candidate_tokens ||
        candidate_count == 0) {
        return 0;
    }
    packed_count = candidate_count > 4U ? 4U : candidate_count;
    memset(candidate_words, 0, candidate_word_count * sizeof(uint64_t));
    candidate_words[0] = decode_step;
    candidate_words[1] = packed_count;
    for (uint64_t i = 0; i < packed_count; ++i) {
        uint64_t base = 2U + i * 7U;

        candidate_words[base] = i;
        candidate_words[base + 1U] = candidate_tokens[i];
        candidate_words[base + 2U] = candidate_logit_bits ? candidate_logit_bits[i] : 0U;
        candidate_words[base + 3U] =
            candidate_text_checksums ? candidate_text_checksums[i] : 0U;
        candidate_words[base + 4U] = candidate_piece_bytes ? candidate_piece_bytes[i] : 0U;
        candidate_words[base + 5U] = candidate_piece_word0 ? candidate_piece_word0[i] : 0U;
        candidate_words[base + 6U] = candidate_piece_word1 ? candidate_piece_word1[i] : 0U;
    }
    candidate_words[31] = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                               31U * sizeof(uint64_t));
    return packed_count;
}

int mem_service_obmm_service_v0_publish_engram_candidates(struct mem_service *svc,
                                                    uint32_t local_node,
                                                    uint32_t cluster_node_count,
                                                    uint64_t decode_step,
                                                    const uint64_t *candidate_tokens,
                                                    const uint64_t *candidate_logit_bits,
                                                    const uint64_t *candidate_text_checksums,
                                                    const uint64_t *candidate_piece_bytes,
                                                    const uint64_t *candidate_piece_word0,
                                                    const uint64_t *candidate_piece_word1,
                                                    uint64_t candidate_count)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record candidates_record;
    char candidates_key[96];
    uint64_t candidate_words[32];
    uint64_t candidates_offset;
    uint64_t candidates_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES;
    uint64_t candidates_checksum;
    uint64_t packed_count;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count || !candidate_tokens || candidate_count == 0) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0) {
        return -1;
    }

    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        mem_service_payload_arena_alloc(rt,
                                  candidates_bytes,
                                  64,
                                  &candidates_offset) != 0) {
        return -1;
    }
    packed_count = mem_service_pack_qwen3_engram_candidates(decode_step,
                                                      candidate_tokens,
                                                      candidate_logit_bits,
                                                      candidate_text_checksums,
                                                      candidate_piece_bytes,
                                                      candidate_piece_word0,
                                                      candidate_piece_word1,
                                                      candidate_count,
                                                      candidate_words,
                                                      32U);
    if (packed_count == 0) {
        return -1;
    }
    candidates_checksum =
        mem_service_checksum_bytes((const uint8_t *)candidate_words, candidates_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + candidates_offset, candidate_words, candidates_bytes);
    if (mem_service_update_region_range_at(local_slot, candidates_offset, candidates_bytes, true) !=
        0) {
        return -1;
    }
    (void)msync(base + candidates_offset, candidates_bytes, MS_SYNC);

    mem_service_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                     candidates_offset,
                                     candidates_bytes,
                                     candidates_checksum,
                                     &candidates_record) != 0 ||
        mem_service_write_cluster_payload(rt, svc, local_slot) != 0) {
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
    if (mem_service_push_obmm_object_descs(rt,
                                     candidates_record.object_payload_kind,
                                     candidates_record.object_backing_offset,
                                     candidates_record.object_backing_len,
                                     candidates_record.object_payload_checksum,
                                     object_epoch) != 0) {
        return -1;
    }

    printf("[mem_service] stage qwen3_engram_candidates_publish local=node%u step=%" PRIu64
           " candidate_count=%" PRIu64
           " candidates_key=%s candidates_version=%" PRIu64
           " candidates_checksum=0x%016" PRIx64
           " epoch=%u seq=%u backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           packed_count,
           candidates_key,
           candidates_record.version,
           candidates_checksum,
           object_epoch,
           local_publish_seq);
    return 0;
}

int mem_service_obmm_service_v0_publish_engram_step(struct mem_service *svc,
                                              uint32_t local_node,
                                              uint32_t cluster_node_count,
                                              uint64_t decode_step,
                                              const uint64_t *history_tokens,
                                              uint64_t history_token_count,
                                              uint64_t raw_sampled_token,
                                              uint64_t runner_up_token,
                                              uint64_t selected_token,
                                              uint64_t blocked_count,
                                              uint64_t fallback_used,
                                              int64_t top_score_milli,
                                              int64_t runner_up_score_milli,
                                              uint64_t no_repeat_ngram_size,
                                              uint64_t repetition_penalty_milli,
                                              uint64_t history_window,
                                              uint64_t logits_checksum,
                                              uint64_t text_checksum)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct mem_service_record records[3];
    char history_key[96];
    char selected_key[96];
    char state_key[96];
    uint64_t history_words[1024 + 2];
    uint64_t selected_words[8];
    uint64_t state_words[16];
    uint64_t history_offset;
    uint64_t selected_offset;
    uint64_t state_offset;
    uint64_t published_history_token_count;
    uint64_t history_bytes;
    uint64_t selected_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES;
    uint64_t state_bytes = MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES;
    uint64_t history_checksum;
    uint64_t selected_checksum;
    uint64_t state_checksum;
    uint16_t local_publish_seq;
    uint16_t object_epoch;
    int owner_idx;
    uint8_t *base;

    if (!svc || cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count || !history_tokens ||
        history_token_count > 1024U) {
        return -1;
    }
    if (history_token_count == UINT64_MAX || history_token_count + 1U > 1024U) {
        return -1;
    }
    if (mem_service_cluster_runtime_require(rt) != 0 ||
        mem_service_publish_qwen3_layer_range_placements(svc, cluster_node_count) != 0) {
        return -1;
    }
    owner_idx = mem_service_qwen3_engram_owner_index(cluster_node_count);
    if (owner_idx < 0 || (uint32_t)owner_idx != local_node) {
        return 0;
    }

    local_slot = &rt->slots[rt->local_idx];
    published_history_token_count = history_token_count + 1U;
    history_bytes = (published_history_token_count + 2U) * sizeof(uint64_t);
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        history_bytes > MEM_SERVICE_OBMM_QWEN3_ENGRAM_HISTORY_BYTES ||
        mem_service_payload_arena_alloc(rt, history_bytes, 64, &history_offset) != 0 ||
        mem_service_payload_arena_alloc(rt, selected_bytes, 64, &selected_offset) != 0 ||
        mem_service_payload_arena_alloc(rt, state_bytes, 64, &state_offset) != 0) {
        return -1;
    }

    memset(history_words, 0, sizeof(history_words));
    history_words[0] = decode_step + 1U;
    history_words[1] = published_history_token_count;
    memcpy(&history_words[2], history_tokens, history_token_count * sizeof(uint64_t));
    history_words[2 + history_token_count] = selected_token;

    memset(selected_words, 0, sizeof(selected_words));
    selected_words[0] = decode_step;
    selected_words[1] = selected_token;
    selected_words[2] = raw_sampled_token;
    selected_words[3] = runner_up_token;
    selected_words[4] = fallback_used;
    selected_words[5] = blocked_count;
    selected_words[6] = logits_checksum;

    history_checksum = mem_service_checksum_bytes((const uint8_t *)history_words, history_bytes);
    selected_checksum = mem_service_checksum_bytes((const uint8_t *)selected_words, selected_bytes);

    memset(state_words, 0, sizeof(state_words));
    state_words[0] = decode_step;
    state_words[1] = published_history_token_count;
    state_words[2] = selected_token;
    state_words[3] = history_checksum;
    state_words[4] = no_repeat_ngram_size;
    state_words[5] = repetition_penalty_milli;
    state_words[6] = blocked_count;
    state_words[7] = fallback_used;
    state_words[8] = raw_sampled_token;
    state_words[9] = runner_up_token;
    state_words[10] = (uint64_t)top_score_milli;
    state_words[11] = (uint64_t)runner_up_score_milli;
    state_words[12] = history_window;
    state_words[13] = logits_checksum;
    state_words[14] = text_checksum;
    state_words[15] = mem_service_checksum_bytes((const uint8_t *)state_words,
                                           15U * sizeof(uint64_t));
    state_checksum = mem_service_checksum_bytes((const uint8_t *)state_words, state_bytes);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + history_offset, history_words, history_bytes);
    memcpy(base + selected_offset, selected_words, selected_bytes);
    memcpy(base + state_offset, state_words, state_bytes);
    if (mem_service_update_region_range_at(local_slot, history_offset, history_bytes, true) != 0 ||
        mem_service_update_region_range_at(local_slot, selected_offset, selected_bytes, true) != 0 ||
        mem_service_update_region_range_at(local_slot, state_offset, state_bytes, true) != 0) {
        return -1;
    }
    (void)msync(base + history_offset, history_bytes, MS_SYNC);
    (void)msync(base + selected_offset, selected_bytes, MS_SYNC);
    (void)msync(base + state_offset, state_bytes, MS_SYNC);

    mem_service_qwen3_engram_history_key(history_key, sizeof(history_key));
    mem_service_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    mem_service_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));

    if (mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY,
                                     history_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                     history_offset,
                                     history_bytes,
                                     history_checksum,
                                     &records[0]) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED,
                                     selected_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                     selected_offset,
                                     selected_bytes,
                                     selected_checksum,
                                     &records[1]) != 0 ||
        mem_service_put_obmm_object_record(svc,
                                     MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE,
                                     state_key,
                                     local_node,
                                     MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                     state_offset,
                                     state_bytes,
                                     state_checksum,
                                     &records[2]) != 0 ||
        mem_service_write_cluster_payload(rt, svc, local_slot) != 0) {
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
    for (size_t i = 0; i < 3U; ++i) {
        if (mem_service_push_obmm_object_descs(rt,
                                         records[i].object_payload_kind,
                                         records[i].object_backing_offset,
                                         records[i].object_backing_len,
                                         records[i].object_payload_checksum,
                                         object_epoch) != 0) {
            return -1;
        }
    }

    printf("[mem_service] stage qwen3_engram_decision_publish local=node%u step=%" PRIu64
           " objects=3 history_tokens=%" PRIu64 " selected_token=%" PRIu64
           " raw_token=%" PRIu64 " runner_up=%" PRIu64
           " fallback=%" PRIu64 " blocked=%" PRIu64
           " top_score_milli=%" PRId64 " runner_up_score_milli=%" PRId64
           " history_window=%" PRIu64
           " history_key=%s history_version=%" PRIu64
           " selected_key=%s state_key=%s"
           " history_checksum=0x%016" PRIx64
           " selected_checksum=0x%016" PRIx64
           " state_checksum=0x%016" PRIx64
           " logits_checksum=0x%016" PRIx64
           " text_checksum=0x%016" PRIx64
           " epoch=%u seq=%u backing=obmm_pool metadata=db queue=obmm_spsc status=ok\n",
           local_node + 1U,
           decode_step,
           published_history_token_count,
           selected_token,
           raw_sampled_token,
           runner_up_token,
           fallback_used,
           blocked_count,
           top_score_milli,
           runner_up_score_milli,
           history_window,
           history_key,
           records[0].version,
           selected_key,
           state_key,
           history_checksum,
           selected_checksum,
           state_checksum,
           logits_checksum,
           text_checksum,
           object_epoch,
           local_publish_seq);
    return 0;
}
