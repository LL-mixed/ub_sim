#include "mem_service_internal.h"

#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"

int mem_service_obmm_service_v0_wait_engram_candidates(struct mem_service *svc,
                                                 uint64_t decode_step,
                                                 uint64_t timeout_ms,
                                                 uint64_t *candidate_tokens_out,
                                                 uint64_t *candidate_logit_bits_out,
                                                 uint64_t *candidate_text_checksums_out,
                                                 uint64_t *candidate_piece_bytes_out,
                                                 uint64_t *candidate_piece_word0_out,
                                                 uint64_t *candidate_piece_word1_out,
                                                 uint64_t candidate_capacity,
                                                 uint64_t *candidate_count_out,
                                                 uint64_t *candidate_checksum_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    char candidates_key[96];
    long deadline;
    int candidate_owner_idx = -1;

    if (!svc || !candidate_tokens_out || !candidate_count_out ||
        candidate_capacity == 0) {
        return -1;
    }
    *candidate_count_out = 0;
    if (candidate_checksum_out) {
        *candidate_checksum_out = 0;
    }
    mem_service_qwen3_engram_candidates_key(decode_step,
                                      candidates_key,
                                      sizeof(candidates_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record candidates_record;
        struct obmm_desc candidates_desc;
        uint64_t candidate_words[32];
        uint64_t candidate_count;
        uint64_t inner_checksum;
        uint64_t candidates_checksum;
        bool candidates_record_found = false;

        candidate_owner_idx = -1;
        memset(&candidates_record, 0, sizeof(candidates_record));
        memset(&candidates_desc, 0, sizeof(candidates_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            rt->node_count > 0) {
            for (int node_idx = 0; node_idx < rt->node_count; ++node_idx) {
                terminal_slot = &rt->slots[node_idx];
                memset(&candidates_record, 0, sizeof(candidates_record));
                if (node_idx == rt->local_idx) {
                    candidates_record_found =
                        mem_service_slot_find_record(terminal_slot,
                                               candidates_key,
                                               &candidates_record);
                } else {
                    struct obmm_desc rx;
                    struct mem_service_cluster_payload_compact_summary compact;
                    struct mem_service_cluster_payload_header seen;

                    if (mem_service_take_pending_object_kind_len_desc(
                            rt,
                            node_idx,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                            &candidates_desc)) {
                    } else if (rt->ingress_queues[node_idx]) {
                        while (obmm_spsc_pop(rt->ingress_queues[node_idx], &rx) == 0) {
                            if (mem_service_object_desc_kind_len_matches(
                                    &rx,
                                    MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES,
                                    MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES)) {
                                candidates_desc = rx;
                                break;
                            }
                            mem_service_stash_pending_desc(rt, node_idx, &rx);
                        }
                    }
                    if (candidates_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                        continue;
                    }
                    if (!terminal_slot->region.addr &&
                        mem_service_activate_remote_slot(rt, node_idx) != 0) {
                        continue;
                    }
                    memset(&compact, 0, sizeof(compact));
                    memset(&seen, 0, sizeof(seen));
                    candidates_record_found =
                        mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                      &compact,
                                                                      &seen) &&
                        mem_service_slot_find_record_by_obmm_object_backing(
                            terminal_slot,
                            MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES,
                            MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
                            candidates_desc.payload_offset,
                            candidates_desc.payload_len,
                            candidates_desc.cookie,
                            &candidates_record);
                }
                if (candidates_record_found &&
                    candidates_record.kind == MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.version == 1U &&
                    candidates_record.object_payload_kind ==
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES &&
                    candidates_record.object_backing_len ==
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES &&
                    terminal_slot->region.addr &&
                    candidates_record.object_backing_offset +
                            candidates_record.object_backing_len <=
                        terminal_slot->region.len) {
                    candidate_owner_idx = node_idx;
                    break;
                }
                memset(&candidates_desc, 0, sizeof(candidates_desc));
            }
            if (candidate_owner_idx < 0) {
                usleep(10000);
                continue;
            }
            terminal_slot = &rt->slots[candidate_owner_idx];
            if (candidates_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.version != 1U ||
                candidates_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES ||
                candidates_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES ||
                !terminal_slot->region.addr ||
                candidates_record.object_backing_offset + candidates_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(candidate_words, 0, sizeof(candidate_words));
            memcpy(candidate_words,
                   (uint8_t *)terminal_slot->region.addr +
                       candidates_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                  31U * sizeof(uint64_t));
            candidates_checksum = mem_service_checksum_bytes((const uint8_t *)candidate_words,
                                                       MEM_SERVICE_OBMM_QWEN3_ENGRAM_CANDIDATES_BYTES);
            candidate_count = candidate_words[1];
            if (candidate_words[0] == decode_step &&
                candidate_count > 0 &&
                candidate_count <= 4U &&
                candidate_count <= candidate_capacity &&
                candidate_words[31] == inner_checksum &&
                candidates_checksum == candidates_record.object_payload_checksum) {
                for (uint64_t i = 0; i < candidate_count; ++i) {
                    uint64_t base = 2U + i * 7U;

                    candidate_tokens_out[i] = candidate_words[base + 1U];
                    if (candidate_logit_bits_out) {
                        candidate_logit_bits_out[i] = candidate_words[base + 2U];
                    }
                    if (candidate_text_checksums_out) {
                        candidate_text_checksums_out[i] = candidate_words[base + 3U];
                    }
                    if (candidate_piece_bytes_out) {
                        candidate_piece_bytes_out[i] = candidate_words[base + 4U];
                    }
                    if (candidate_piece_word0_out) {
                        candidate_piece_word0_out[i] = candidate_words[base + 5U];
                    }
                    if (candidate_piece_word1_out) {
                        candidate_piece_word1_out[i] = candidate_words[base + 6U];
                    }
                }
                *candidate_count_out = candidate_count;
                if (candidate_checksum_out) {
                    *candidate_checksum_out = candidates_checksum;
                }
                printf("[mem_service] stage qwen3_engram_candidates_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " candidate_count=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       candidates_key,
                       candidate_owner_idx + 1,
                       candidates_record.version,
                       candidate_count,
                       candidates_record.object_backing_len,
                       candidates_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_candidates_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           candidates_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_selected_token(struct mem_service *svc,
                                                     uint64_t decode_step,
                                                     uint64_t timeout_ms,
                                                     uint64_t *selected_token_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    char selected_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc) {
        return -1;
    }
    mem_service_qwen3_engram_selected_key(decode_step, selected_key, sizeof(selected_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record selected_record;
        struct obmm_desc selected_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        uint16_t expected_epoch;
        bool selected_record_found = false;

        memset(&selected_record, 0, sizeof(selected_record));
        memset(&selected_desc, 0, sizeof(selected_desc));
        expected_epoch = (uint16_t)(decode_step + 1U);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                selected_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           selected_key,
                                           &selected_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                        &selected_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES)) {
                            selected_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (selected_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                selected_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
                        selected_desc.payload_offset,
                        selected_desc.payload_len,
                        selected_desc.cookie,
                        &selected_record);
            }
            if (!selected_record_found ||
                selected_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED ||
                selected_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES ||
                !terminal_slot->region.addr ||
                selected_record.object_backing_offset + selected_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       selected_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            MEM_SERVICE_OBMM_QWEN3_ENGRAM_SELECTED_BYTES);
            if (payload_words[0] == decode_step &&
                checksum == selected_record.object_payload_checksum) {
                if (selected_token_out) {
                    *selected_token_out = payload_words[1];
                }
                printf("[mem_service] stage qwen3_engram_selected_token_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " bytes=%" PRIu64 " token=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       selected_key,
                       owner_idx + 1,
                       selected_record.version,
                       selected_record.object_backing_len,
                       payload_words[1],
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_selected_token_wait=timeout step=%" PRIu64
           " object_key=%s\n",
           decode_step,
           selected_key);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_history(struct mem_service *svc,
                                              uint64_t decode_step,
                                              uint64_t timeout_ms,
                                              uint64_t *history_tokens_out,
                                              uint64_t history_token_capacity,
                                              uint64_t *history_token_count_out,
                                              uint64_t *history_checksum_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    char history_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());
    uint64_t expected_version = decode_step + 1U;

    if (!svc || !history_tokens_out || history_token_capacity == 0 ||
        !history_token_count_out) {
        return -1;
    }
    *history_token_count_out = 0;
    if (history_checksum_out) {
        *history_checksum_out = 0;
    }
    mem_service_qwen3_engram_history_key(history_key, sizeof(history_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record history_record;
        struct obmm_desc history_desc;
        uint64_t payload_words[1024 + 2];
        uint64_t checksum;
        uint64_t history_token_count;
        uint64_t expected_bytes;
        uint16_t expected_epoch;
        bool history_record_found = false;

        memset(&history_record, 0, sizeof(history_record));
        memset(&history_desc, 0, sizeof(history_desc));
        expected_epoch = (uint16_t)expected_version;
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                history_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           history_key,
                                           &history_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                if (mem_service_take_pending_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        3U * sizeof(uint64_t),
                        sizeof(payload_words),
                        &history_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                                3U * sizeof(uint64_t),
                                sizeof(payload_words))) {
                            history_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (history_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                history_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
                        history_desc.payload_offset,
                        history_desc.payload_len,
                        history_desc.cookie,
                        &history_record);
            }
            if (!history_record_found ||
                history_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_HISTORY ||
                history_record.version != expected_version ||
                history_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY ||
                history_record.object_backing_len < 3U * sizeof(uint64_t) ||
                history_record.object_backing_len > sizeof(payload_words) ||
                !terminal_slot->region.addr ||
                history_record.object_backing_offset + history_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(payload_words, 0, sizeof(payload_words));
            memcpy(payload_words,
                   (uint8_t *)terminal_slot->region.addr +
                       history_record.object_backing_offset,
                   history_record.object_backing_len);
            checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            history_record.object_backing_len);
            history_token_count = payload_words[1];
            expected_bytes = (history_token_count + 2U) * sizeof(uint64_t);
            if (payload_words[0] == expected_version &&
                history_token_count > 0 &&
                history_token_count <= history_token_capacity &&
                expected_bytes == history_record.object_backing_len &&
                checksum == history_record.object_payload_checksum) {
                memcpy(history_tokens_out,
                       &payload_words[2],
                       history_token_count * sizeof(uint64_t));
                *history_token_count_out = history_token_count;
                if (history_checksum_out) {
                    *history_checksum_out = checksum;
                }
                printf("[mem_service] stage qwen3_engram_history_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       history_key,
                       owner_idx + 1,
                       history_record.version,
                       history_token_count,
                       history_record.object_backing_len,
                       checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_history_wait=timeout step=%" PRIu64
           " object_key=%s expected_version=%" PRIu64 "\n",
           decode_step,
           history_key,
           expected_version);
    return -1;
}

int mem_service_obmm_service_v0_wait_engram_state(struct mem_service *svc,
                                            uint64_t decode_step,
                                            uint64_t timeout_ms,
                                            uint64_t expected_history_token_count,
                                            uint64_t expected_selected_token,
                                            uint64_t expected_history_checksum,
                                            uint64_t no_repeat_ngram_size,
                                            uint64_t repetition_penalty_milli,
                                            uint64_t *state_checksum_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    char state_key[96];
    long deadline;
    int owner_idx = mem_service_qwen3_engram_owner_index(mem_service_qwen3_range_nodes());

    if (!svc || expected_history_token_count == 0) {
        return -1;
    }
    if (state_checksum_out) {
        *state_checksum_out = 0;
    }
    mem_service_qwen3_engram_state_key(decode_step, state_key, sizeof(state_key));
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        struct mem_service_cluster_slot *terminal_slot;
        struct mem_service_record state_record;
        struct obmm_desc state_desc;
        uint64_t state_words[16];
        uint64_t inner_checksum;
        uint64_t state_checksum;
        bool state_record_found = false;

        memset(&state_record, 0, sizeof(state_record));
        memset(&state_desc, 0, sizeof(state_desc));
        if (mem_service_cluster_runtime_require(rt) == 0 &&
            owner_idx >= 0 &&
            owner_idx < rt->node_count) {
            terminal_slot = &rt->slots[owner_idx];
            if (owner_idx == rt->local_idx) {
                state_record_found =
                    mem_service_slot_find_record(terminal_slot,
                                           state_key,
                                           &state_record);
            } else {
                struct obmm_desc rx;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint16_t expected_epoch = (uint16_t)(decode_step + 1U);

                if (expected_epoch == 0) {
                    expected_epoch = 1;
                }
                if (mem_service_take_pending_object_desc(
                        rt,
                        owner_idx,
                        expected_epoch,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                        &state_desc)) {
                } else if (rt->ingress_queues[owner_idx]) {
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_object_desc_matches(
                                &rx,
                                expected_epoch,
                                MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES,
                                MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES)) {
                            state_desc = rx;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
                if (state_desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
                    usleep(10000);
                    continue;
                }
                if (!terminal_slot->region.addr &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    usleep(10000);
                    continue;
                }
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                state_record_found =
                    mem_service_try_read_stable_compact_summary_region(terminal_slot,
                                                                  &compact,
                                                                  &seen) &&
                    mem_service_slot_find_record_by_obmm_object_backing(
                        terminal_slot,
                        MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE,
                        MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
                        state_desc.payload_offset,
                        state_desc.payload_len,
                        state_desc.cookie,
                        &state_record);
            }
            if (!state_record_found ||
                state_record.kind != MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE ||
                state_record.version != 1U ||
                state_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE ||
                state_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES ||
                !terminal_slot->region.addr ||
                state_record.object_backing_offset + state_record.object_backing_len >
                    terminal_slot->region.len) {
                usleep(10000);
                continue;
            }
            memset(state_words, 0, sizeof(state_words));
            memcpy(state_words,
                   (uint8_t *)terminal_slot->region.addr +
                       state_record.object_backing_offset,
                   MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            inner_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  15U * sizeof(uint64_t));
            state_checksum = mem_service_checksum_bytes((const uint8_t *)state_words,
                                                  MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES);
            if (state_words[0] == decode_step &&
                state_words[1] == expected_history_token_count &&
                state_words[2] == expected_selected_token &&
                state_words[3] == expected_history_checksum &&
                state_words[4] == no_repeat_ngram_size &&
                state_words[5] == repetition_penalty_milli &&
                state_words[15] == inner_checksum &&
                state_checksum == state_record.object_payload_checksum) {
                if (state_checksum_out) {
                    *state_checksum_out = state_checksum;
                }
                printf("[mem_service] stage qwen3_engram_state_wait step=%" PRIu64
                       " object_key=%s owner=node%d version=%" PRIu64
                       " history_tokens=%" PRIu64 " selected_token=%" PRIu64
                       " history_checksum=0x%016" PRIx64
                       " blocked=%" PRIu64 " fallback=%" PRIu64
                       " raw_token=%" PRIu64 " runner_up=%" PRIu64
                       " top_score_milli=%" PRId64
                       " runner_up_score_milli=%" PRId64
                       " history_window=%" PRIu64
                       " logits_checksum=0x%016" PRIx64
                       " text_checksum=0x%016" PRIx64
                       " bytes=%" PRIu64
                       " checksum=0x%016" PRIx64
                       " source=obmm_object_service status=ok\n",
                       decode_step,
                       state_key,
                       owner_idx + 1,
                       state_record.version,
                       state_words[1],
                       state_words[2],
                       state_words[3],
                       state_words[6],
                       state_words[7],
                       state_words[8],
                       state_words[9],
                       (int64_t)state_words[10],
                       (int64_t)state_words[11],
                       state_words[12],
                       state_words[13],
                       state_words[14],
                       state_record.object_backing_len,
                       state_checksum);
                return 0;
            }
        }
        usleep(10000);
    }
    printf("[mem_service] gap qwen3_engram_state_wait=timeout step=%" PRIu64
           " object_key=%s expected_history_tokens=%" PRIu64
           " expected_selected_token=%" PRIu64
           " expected_history_checksum=0x%016" PRIx64 "\n",
           decode_step,
           state_key,
           expected_history_token_count,
           expected_selected_token,
           expected_history_checksum);
    return -1;
}
