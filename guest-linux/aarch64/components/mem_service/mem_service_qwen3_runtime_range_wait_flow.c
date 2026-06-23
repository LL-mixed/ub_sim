#include "mem_service_internal.h"

#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"

static int mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    bool allow_terminal_commit,
    struct mem_service_object_payload_view *view_out)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_qwen3_layer_range_placement source_placement;
    struct mem_service_cluster_slot *source_slot = NULL;
    struct mem_service_record remote_hidden_output;
    struct obmm_desc handoff_desc;
    struct obmm_desc terminal_desc;
    char ingress_key[96];
    char token_result_key[96];
    long deadline;
    long wait_enter_ms;
    long found_local_ms = 0;
    long found_ms = 0;
    long checksum_ms;
    long producer_publish_ms;
    long producer_publish_monotonic_ms;
    long producer_clock_offset_ms;
    long producer_to_found_ms = 0;
    long producer_to_found_monotonic_ms = 0;
    uint64_t activate_ms = 0;
    uint64_t metadata_ms = 0;
    uint32_t attempts = 0;
    uint16_t expected_epoch;
    unsigned int relax_attempt = 0;
    uint32_t source_node = UINT32_MAX;
    uint32_t terminal_source_node = UINT32_MAX;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);
    const uint8_t *payload_view;
    uint64_t checksum;
    bool terminal_desc_found = false;

    if (!view_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;
    if (local_placement.layer_start == 0 && decode_step == 0) {
        return 0;
    }
    if (local_placement.layer_start == 0) {
        struct mem_service_record token_record;
        struct mem_service_cluster_slot *owner_slot;
        char token_result_key[96];
        struct obmm_desc token_desc;
        uint64_t payload_words[8];
        uint64_t checksum;
        bool token_input_found = false;
        bool token_desc_found = false;

        if (mem_service_cluster_runtime_require(rt) != 0) {
            return -1;
        }
        memset(&token_desc, 0, sizeof(token_desc));
        wait_enter_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        token_result_key[0] = '\0';
        expected_epoch = (uint16_t)(decode_step & 0xffffU);
        if (expected_epoch == 0) {
            expected_epoch = 1;
        }
        deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
        while (obmm_now_ms() < deadline) {
            int owner_idx;

            attempts++;
            for (owner_idx = 0;
                 !token_desc_found && owner_idx < rt->node_count;
                 ++owner_idx) {
                struct obmm_desc rx;

                if (mem_service_take_pending_qwen3_token_result_desc(rt,
                                                               owner_idx,
                                                               expected_epoch,
                                                               &token_desc)) {
                    source_node = (uint32_t)owner_idx;
                    token_desc_found = true;
                    break;
                }
                if (owner_idx == rt->local_idx ||
                    !rt->ingress_queues[owner_idx]) {
                    continue;
                }
                while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                    if (mem_service_qwen3_token_result_desc_matches(&rx,
                                                              expected_epoch)) {
                        token_desc = rx;
                        source_node = (uint32_t)owner_idx;
                        token_desc_found = true;
                        break;
                    }
                    mem_service_stash_pending_desc(rt, owner_idx, &rx);
                }
            }
            if (!token_desc_found) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            if (!rt->slots[source_node].region.addr) {
                long activate_start_ms = obmm_now_ms();

                if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
            }
            owner_slot = &rt->slots[source_node];
            memset(&token_record, 0, sizeof(token_record));
            {
                long metadata_start_ms = obmm_now_ms();
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;

                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!owner_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(owner_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record_by_obmm_object_backing(
                        owner_slot,
                        MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                        token_desc.payload_offset,
                        token_desc.payload_len,
                        token_desc.cookie,
                        &token_record)) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
            }
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "%s",
                     token_record.key);
            if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                token_record.object_backing_offset != token_desc.payload_offset ||
                token_record.object_backing_len != token_desc.payload_len ||
                token_desc.cookie !=
                    (uint32_t)(token_record.object_payload_checksum ^
                               (token_record.object_payload_checksum >> 32)) ||
                token_record.object_backing_offset > owner_slot->region.len ||
                token_record.object_backing_len >
                    owner_slot->region.len - token_record.object_backing_offset) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_invalid local=node%u source=node%u key=%s\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key);
                return -1;
            }
            payload_view =
                (const uint8_t *)owner_slot->region.addr +
                token_record.object_backing_offset;
            memcpy(payload_words,
                   payload_view,
                   MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (payload_words[0] != decode_step - 1U) {
                printf("[mem_service] gap qwen3_range_forward=runtime_token_input_step_mismatch local=node%u source=node%u key=%s got=%" PRIu64 " expected=%" PRIu64 "\n",
                       local_node + 1U,
                       source_node + 1U,
                       token_result_key,
                       payload_words[0],
                       decode_step - 1U);
                return -1;
            }
            checksum = mem_service_qwen3_hidden_payload_checksum(
                payload_view,
                MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
            if (checksum != token_record.object_payload_checksum) {
                mem_service_cpu_relax_wait(&relax_attempt);
                continue;
            }
            token_input_found = true;
            break;
        }
        if (!token_input_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_token_input_wait_failed local=node%u source=%s step=%" PRIu64 " epoch=%u attempts=%u desc_found=%u\n",
                   local_node + 1U,
                   source_node == UINT32_MAX ? "none" : "descriptor",
                   decode_step - 1U,
                   expected_epoch,
                   attempts,
                   token_desc_found ? 1U : 0U);
            return -1;
        }
        found_local_ms = obmm_now_ms();
        found_ms = mem_service_wallclock_ms();
        producer_publish_ms = (long)token_record.last_result_segment;
        producer_publish_monotonic_ms =
            (long)token_record.object_publish_monotonic_ms;
        producer_clock_offset_ms =
            (long)token_record.object_publish_supernode_offset_ms;
        if (producer_publish_ms > 0 && found_ms > 0) {
            producer_to_found_ms = found_ms - producer_publish_ms;
        }
        if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
            producer_to_found_monotonic_ms =
                found_local_ms - producer_publish_monotonic_ms;
        }
        memset(view_out, 0, sizeof(*view_out));
        view_out->data = payload_view;
        view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
        view_out->checksum = checksum;
        view_out->owner_node = source_node;
        view_out->payload_kind = token_record.object_payload_kind;
        view_out->backing_offset = token_record.object_backing_offset;
        memcpy(view_out->token_result_words,
               payload_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                            &view_out->object_ref) != 0) {
            return -1;
        }
        view_out->wait_enter_monotonic_ms =
            wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
        view_out->found_monotonic_ms =
            found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
        view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
        view_out->producer_publish_supernode_ms =
            producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
        view_out->producer_publish_monotonic_ms =
            producer_publish_monotonic_ms > 0 ?
                (uint64_t)producer_publish_monotonic_ms :
                0;
        view_out->producer_clock_offset_ms = producer_clock_offset_ms;
        view_out->producer_to_found_supernode_ms = producer_to_found_ms;
        view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
        view_out->source_node = source_node;
        view_out->wait_attempts = attempts;
        view_out->activate_ms = activate_ms;
        view_out->metadata_ms = metadata_ms;
        printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " token=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=0 validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem source=terminal_token_result target=mapped_view status=ok\n",
               local_node + 1U,
               source_node + 1U,
               token_result_key,
               view_out->object_ref.key_hash,
               view_out->object_ref.object_version,
               local_placement.layer_start,
               local_placement.layer_end,
               checksum,
               view_out->len,
               payload_words[1],
               found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
               producer_publish_ms,
               producer_publish_monotonic_ms,
               producer_clock_offset_ms,
               producer_to_found_ms,
               producer_to_found_monotonic_ms,
               view_out->wait_attempts,
               activate_ms,
               metadata_ms);
        return 0;
    }
    memset(&source_placement, 0, sizeof(source_placement));
    source_placement.owner_node = local_node - 1U;
    source_placement.next_owner_node = local_node;
    source_placement.terminal = false;
    if (mem_service_qwen3_layer_range_for_node(source_placement.owner_node,
                                         cluster_node_count,
                                         &source_placement.layer_start,
                                         &source_placement.layer_end,
                                         &source_placement.next_owner_node) != 0 ||
        source_placement.next_owner_node != local_node) {
        return -1;
    }
    source_placement.layer_count =
        source_placement.layer_end - source_placement.layer_start;
    source_node = source_placement.owner_node;
    if (mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (source_node >= cluster_node_count || !rt->ingress_queues[source_node]) {
        return -1;
    }
    ingress_key[0] = '\0';

    memset(&remote_hidden_output, 0, sizeof(remote_hidden_output));
    memset(&handoff_desc, 0, sizeof(handoff_desc));
    memset(&terminal_desc, 0, sizeof(terminal_desc));
    expected_epoch = (uint16_t)((decode_step + 1U) & 0xffffU);
    if (expected_epoch == 0) {
        expected_epoch = 1;
    }
    wait_enter_ms = obmm_now_ms();
    deadline = wait_enter_ms + mem_service_qwen3_runtime_range_wait_ms();
    while (obmm_now_ms() < deadline) {
        struct obmm_desc rx;

        attempts++;
        if (allow_terminal_commit) {
            snprintf(token_result_key,
                     sizeof(token_result_key),
                     "tokens/%s/decode-step%" PRIu64,
                     mem_service_qwen3_model_key(),
                     decode_step);
            if (!terminal_desc_found) {
                for (int owner_idx = 0;
                     !terminal_desc_found && owner_idx < rt->node_count;
                     ++owner_idx) {
                    struct obmm_desc rx;

                    if (mem_service_take_pending_qwen3_token_result_desc(
                            rt,
                            owner_idx,
                            expected_epoch,
                            &terminal_desc)) {
                        terminal_source_node = (uint32_t)owner_idx;
                        terminal_desc_found = true;
                        break;
                    }
                    if (owner_idx == rt->local_idx ||
                        !rt->ingress_queues[owner_idx]) {
                        continue;
                    }
                    while (obmm_spsc_pop(rt->ingress_queues[owner_idx], &rx) == 0) {
                        if (mem_service_qwen3_token_result_desc_matches(
                                &rx,
                                expected_epoch)) {
                            terminal_desc = rx;
                            terminal_source_node = (uint32_t)owner_idx;
                            terminal_desc_found = true;
                            break;
                        }
                        mem_service_stash_pending_desc(rt, owner_idx, &rx);
                    }
                }
            }
            if (terminal_desc_found &&
                terminal_source_node < (uint32_t)rt->node_count) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (terminal_source_node != (uint32_t)rt->local_idx &&
                    !rt->slots[terminal_source_node].region.addr) {
                    long activate_start_ms = obmm_now_ms();

                    if (mem_service_activate_remote_slot(
                            rt,
                            (int)terminal_source_node) != 0) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    activate_ms +=
                        (uint64_t)(obmm_now_ms() - activate_start_ms);
                }
                token_slot = &rt->slots[terminal_source_node];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                {
                    long metadata_start_ms = obmm_now_ms();

                    if (!token_slot->region.addr ||
                        !mem_service_try_read_stable_compact_summary_region(
                            token_slot,
                            &compact,
                            &seen) ||
                        !mem_service_slot_find_record_by_obmm_object_backing(
                            token_slot,
                            MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT,
                            MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
                            terminal_desc.payload_offset,
                            terminal_desc.payload_len,
                            terminal_desc.cookie,
                            &token_record)) {
                        mem_service_cpu_relax_wait(&relax_attempt);
                        continue;
                    }
                    metadata_ms +=
                        (uint64_t)(obmm_now_ms() - metadata_start_ms);
                }
                if (token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind !=
                        MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len !=
                        MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset !=
                        terminal_desc.payload_offset ||
                    token_record.object_backing_len != terminal_desc.payload_len ||
                    terminal_desc.cookie !=
                        (uint32_t)(token_record.object_payload_checksum ^
                                   (token_record.object_payload_checksum >> 32)) ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len -
                            token_record.object_backing_offset) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    mem_service_cpu_relax_wait(&relax_attempt);
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = terminal_source_node;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = terminal_source_node;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%u step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result"
                       " target=decode_round_scheduler receive=descriptor"
                       " status=committed\n",
                       local_node + 1U,
                       terminal_source_node + 1U,
                       decode_step,
                       payload_words[1],
                       token_record.key,
                       token_checksum);
                return 0;
            }
            for (int owner_idx = 0; owner_idx < rt->node_count; ++owner_idx) {
                struct mem_service_cluster_slot *token_slot;
                struct mem_service_record token_record;
                struct mem_service_cluster_payload_compact_summary compact;
                struct mem_service_cluster_payload_header seen;
                uint64_t payload_words[8];
                uint64_t token_checksum;

                if (owner_idx != rt->local_idx &&
                    mem_service_activate_remote_slot(rt, owner_idx) != 0) {
                    continue;
                }
                token_slot = &rt->slots[owner_idx];
                memset(&token_record, 0, sizeof(token_record));
                memset(&compact, 0, sizeof(compact));
                memset(&seen, 0, sizeof(seen));
                if (!token_slot->region.addr ||
                    !mem_service_try_read_stable_compact_summary_region(token_slot,
                                                                  &compact,
                                                                  &seen) ||
                    !mem_service_slot_find_record(token_slot,
                                            token_result_key,
                                            &token_record) ||
                    token_record.kind != MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT ||
                    token_record.object_payload_kind != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
                    token_record.object_backing_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES ||
                    token_record.object_backing_offset > token_slot->region.len ||
                    token_record.object_backing_len >
                        token_slot->region.len - token_record.object_backing_offset) {
                    continue;
                }
                payload_view =
                    (const uint8_t *)token_slot->region.addr +
                    token_record.object_backing_offset;
                memcpy(payload_words,
                       payload_view,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (payload_words[0] != decode_step) {
                    continue;
                }
                token_checksum = mem_service_qwen3_hidden_payload_checksum(
                    payload_view,
                    MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (token_checksum != token_record.object_payload_checksum) {
                    continue;
                }
                found_local_ms = obmm_now_ms();
                found_ms = mem_service_wallclock_ms();
                producer_publish_ms = (long)token_record.last_result_segment;
                producer_publish_monotonic_ms =
                    (long)token_record.object_publish_monotonic_ms;
                producer_clock_offset_ms =
                    (long)token_record.object_publish_supernode_offset_ms;
                if (producer_publish_ms > 0 && found_ms > 0) {
                    producer_to_found_ms = found_ms - producer_publish_ms;
                }
                if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
                    producer_to_found_monotonic_ms =
                        found_local_ms - producer_publish_monotonic_ms;
                }
                memset(view_out, 0, sizeof(*view_out));
                view_out->data = payload_view;
                view_out->len = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES;
                view_out->checksum = token_checksum;
                view_out->owner_node = (uint32_t)owner_idx;
                view_out->payload_kind = token_record.object_payload_kind;
                view_out->backing_offset = token_record.object_backing_offset;
                memcpy(view_out->token_result_words,
                       payload_words,
                       MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
                if (mem_service_record_to_lingqu_obmm_ref(&token_record,
                                                    &view_out->object_ref) != 0) {
                    return -1;
                }
                view_out->wait_enter_monotonic_ms =
                    wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
                view_out->found_monotonic_ms =
                    found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
                view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
                view_out->producer_publish_supernode_ms =
                    producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
                view_out->producer_publish_monotonic_ms =
                    producer_publish_monotonic_ms > 0 ?
                        (uint64_t)producer_publish_monotonic_ms :
                        0;
                view_out->producer_clock_offset_ms = producer_clock_offset_ms;
                view_out->producer_to_found_supernode_ms = producer_to_found_ms;
                view_out->producer_to_found_monotonic_ms =
                    producer_to_found_monotonic_ms;
                view_out->source_node = (uint32_t)owner_idx;
                view_out->wait_attempts = attempts;
                view_out->activate_ms = activate_ms;
                view_out->metadata_ms = metadata_ms;
                printf("[mem_service] stage qwen3_decode_round_terminal_committed"
                       " local=node%u source=node%d step=%" PRIu64
                       " token=%" PRIu64 " object_key=%s"
                       " checksum=0x%016" PRIx64
                       " source=terminal_token_result target=decode_round_scheduler"
                       " status=committed\n",
                       local_node + 1U,
                       owner_idx + 1,
                       decode_step,
                       payload_words[1],
                       token_result_key,
                       token_checksum);
                return 0;
            }
        }
        if (mem_service_take_pending_runtime_range_input_desc(rt,
                                                        (int)source_node,
                                                        expected_epoch,
                                                        &handoff_desc)) {
            break;
        }
        while (obmm_spsc_pop(rt->ingress_queues[source_node], &rx) == 0) {
            if (mem_service_runtime_range_input_desc_matches(&rx,
                                                       expected_epoch)) {
                handoff_desc = rx;
                break;
            }
            mem_service_stash_pending_desc(rt, (int)source_node, &rx);
        }
        if (handoff_desc.type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT) {
            break;
        }
        mem_service_cpu_relax_wait(&relax_attempt);
    }
    if (!mem_service_runtime_range_input_desc_matches(&handoff_desc,
                                                expected_epoch)) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_desc_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               attempts);
        return -1;
    }
    found_local_ms = obmm_now_ms();
    found_ms = mem_service_wallclock_ms();
    if (!rt->slots[source_node].region.addr) {
        long activate_start_ms = obmm_now_ms();

        if (mem_service_activate_remote_slot(rt, (int)source_node) != 0) {
            return -1;
        }
        activate_ms += (uint64_t)(obmm_now_ms() - activate_start_ms);
    }
    {
        bool metadata_found = false;

        source_slot = &rt->slots[source_node];
        while (obmm_now_ms() < deadline) {
            long metadata_start_ms = obmm_now_ms();
            struct mem_service_cluster_payload_compact_summary compact;
            struct mem_service_cluster_payload_header seen;

            memset(&compact, 0, sizeof(compact));
            memset(&seen, 0, sizeof(seen));
            if (mem_service_try_read_stable_compact_summary_region(source_slot,
                                                             &compact,
                                                             &seen) &&
                mem_service_slot_find_record_by_obmm_object_backing(
                    source_slot,
                    MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT,
                    MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
                    handoff_desc.payload_offset,
                    handoff_desc.payload_len,
                    handoff_desc.cookie,
                    &remote_hidden_output)) {
                metadata_ms = (uint64_t)(obmm_now_ms() - metadata_start_ms);
                metadata_found = true;
                break;
            }
            mem_service_cpu_relax_wait(&relax_attempt);
        }
        if (!metadata_found) {
            printf("[mem_service] gap qwen3_range_forward=runtime_ingress_metadata_wait_failed local=node%u source=node%u step=%" PRIu64 " attempts=%u offset=0x%016" PRIx64 " bytes=%" PRIu64 "\n",
                   local_node + 1U,
                   source_node + 1U,
                   decode_step,
                   attempts,
                   handoff_desc.payload_offset,
                   (uint64_t)handoff_desc.payload_len);
            return -1;
        }
    }
    snprintf(ingress_key,
             sizeof(ingress_key),
             "%s",
             remote_hidden_output.key);
    if (!source_slot ||
        remote_hidden_output.object_payload_kind !=
            MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        remote_hidden_output.object_backing_len != hidden_range_bytes ||
        remote_hidden_output.object_backing_offset != handoff_desc.payload_offset ||
        remote_hidden_output.object_backing_len != handoff_desc.payload_len ||
        handoff_desc.cookie !=
            (uint32_t)(remote_hidden_output.object_payload_checksum ^
                       (remote_hidden_output.object_payload_checksum >> 32)) ||
        !source_slot->region.addr ||
        remote_hidden_output.object_backing_offset + remote_hidden_output.object_backing_len >
            source_slot->region.len) {
        printf("[mem_service] gap qwen3_range_forward=runtime_ingress_wait_failed local=node%u source=node%u step=%" PRIu64 " key=%s\n",
               local_node + 1U,
               source_node + 1U,
               decode_step,
               ingress_key);
        return -1;
    }
    payload_view =
        (const uint8_t *)source_slot->region.addr +
        remote_hidden_output.object_backing_offset;
    checksum = remote_hidden_output.object_payload_checksum;
    checksum_ms = 0;
    producer_publish_ms = (long)remote_hidden_output.last_result_segment;
    producer_publish_monotonic_ms =
        (long)remote_hidden_output.object_publish_monotonic_ms;
    producer_clock_offset_ms =
        (long)remote_hidden_output.object_publish_supernode_offset_ms;
    if (producer_publish_ms > 0 && found_ms > 0) {
        producer_to_found_ms = found_ms - producer_publish_ms;
    }
    if (producer_publish_monotonic_ms > 0 && found_local_ms > 0) {
        producer_to_found_monotonic_ms =
            found_local_ms - producer_publish_monotonic_ms;
    }
    memset(view_out, 0, sizeof(*view_out));
    view_out->data = payload_view;
    view_out->len = hidden_range_bytes;
    view_out->checksum = checksum;
    view_out->owner_node = source_node;
    view_out->payload_kind = remote_hidden_output.object_payload_kind;
    view_out->backing_offset = remote_hidden_output.object_backing_offset;
    if (mem_service_record_to_lingqu_obmm_ref(&remote_hidden_output,
                                        &view_out->object_ref) != 0) {
        return -1;
    }
    view_out->wait_enter_monotonic_ms =
        wait_enter_ms > 0 ? (uint64_t)wait_enter_ms : 0;
    view_out->found_monotonic_ms =
        found_local_ms > 0 ? (uint64_t)found_local_ms : 0;
    view_out->ready_monotonic_ms = (uint64_t)obmm_now_ms();
    view_out->producer_publish_supernode_ms =
        producer_publish_ms > 0 ? (uint64_t)producer_publish_ms : 0;
    view_out->producer_publish_monotonic_ms =
        producer_publish_monotonic_ms > 0 ?
            (uint64_t)producer_publish_monotonic_ms :
            0;
    view_out->producer_clock_offset_ms = producer_clock_offset_ms;
    view_out->producer_to_found_supernode_ms = producer_to_found_ms;
    view_out->producer_to_found_monotonic_ms = producer_to_found_monotonic_ms;
    view_out->source_node = source_node;
    view_out->wait_attempts = attempts;
    view_out->activate_ms = activate_ms;
    view_out->metadata_ms = metadata_ms;
    printf("[mem_service] stage qwen3_range_forward_runtime_input_resolve local=node%u source=node%u key=%s key_hash=0x%016" PRIx64 " version=%" PRIu64 " layers=[%u,%u) input_checksum=0x%016" PRIx64 " bytes=%" PRIu64 " wait_enter_to_found_ms=%ld producer_publish_ms=%ld producer_publish_mono_ms=%ld producer_clock_offset_ms=%ld producer_to_found_ms=%ld producer_to_found_mono_ms=%ld attempts=%u activate_ms=%" PRIu64 " metadata_ms=%" PRIu64 " copy_ms=0 checksum_ms=%ld validation=object_desc_backing queue=obmm_spsc receive=descriptor metadata=lingqu_object_service backing=obmm_shmem target=mapped_view status=ok\n",
           local_node + 1U,
           source_node + 1U,
           ingress_key,
           view_out->object_ref.key_hash,
           view_out->object_ref.object_version,
           local_placement.layer_start,
           local_placement.layer_end,
           checksum,
           hidden_range_bytes,
           found_local_ms > 0 ? found_local_ms - wait_enter_ms : -1L,
           producer_publish_ms,
           producer_publish_monotonic_ms,
           producer_clock_offset_ms,
           producer_to_found_ms,
           producer_to_found_monotonic_ms,
           attempts,
           activate_ms,
           metadata_ms,
           checksum_ms);
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input_view(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_object_payload_view *view_out)
{
    return mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
        local_node,
        cluster_node_count,
        decode_step,
        false,
        view_out);
}

int mem_service_obmm_service_v0_wait_scheduler_work_item(
    uint32_t local_node,
    uint32_t cluster_node_count,
    uint64_t decode_step,
    struct mem_service_scheduler_work_item *item_out)
{
    struct mem_service_qwen3_layer_range_placement local_placement;
    struct mem_service_object_payload_view view;

    if (!item_out ||
        cluster_node_count != mem_service_qwen3_range_nodes() ||
        local_node >= cluster_node_count) {
        return -1;
    }
    memset(item_out, 0, sizeof(*item_out));
    memset(&local_placement, 0, sizeof(local_placement));
    local_placement.owner_node = local_node;
    local_placement.next_owner_node = (local_node + 1U) % cluster_node_count;
    local_placement.terminal = (local_node + 1U == cluster_node_count);
    if (mem_service_qwen3_layer_range_for_node(local_node,
                                         cluster_node_count,
                                         &local_placement.layer_start,
                                         &local_placement.layer_end,
                                         &local_placement.next_owner_node) != 0) {
        return -1;
    }
    local_placement.layer_count =
        local_placement.layer_end - local_placement.layer_start;
    local_placement.terminal =
        local_placement.next_owner_node < local_placement.owner_node;

    if (local_placement.layer_start == 0 && decode_step == 0) {
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
        return 0;
    }

    memset(&view, 0, sizeof(view));
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view_internal(
            local_node,
            cluster_node_count,
            decode_step,
            local_placement.layer_start > 0,
            &view) != 0 ||
        !view.data) {
        return -1;
    }
    if (view.payload_kind == MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT &&
        view.len == MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES) {
        uint64_t token_words[8];

        if (local_placement.layer_start == 0) {
            item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
            item_out->range_input = view;
            return 0;
        }
        memcpy(token_words,
               view.token_result_words,
               MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES);
        if (token_words[0] != decode_step) {
            printf("[mem_service] gap qwen3_scheduler_work_item=terminal_step_mismatch local=node%u got=%" PRIu64 " expected=%" PRIu64 "\n",
                   local_node + 1U,
                   token_words[0],
                   decode_step);
            return -1;
        }
        item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_NO_DISPATCH;
        item_out->terminal_step = token_words[0];
        item_out->terminal_token = token_words[1];
        item_out->terminal_owner_node = view.source_node;
        item_out->checksum = view.checksum;
        item_out->wait_enter_monotonic_ms = view.wait_enter_monotonic_ms;
        item_out->found_monotonic_ms = view.found_monotonic_ms;
        item_out->ready_monotonic_ms = view.ready_monotonic_ms;
        item_out->producer_publish_supernode_ms =
            view.producer_publish_supernode_ms;
        item_out->producer_publish_monotonic_ms =
            view.producer_publish_monotonic_ms;
        item_out->producer_clock_offset_ms = view.producer_clock_offset_ms;
        item_out->producer_to_found_supernode_ms =
            view.producer_to_found_supernode_ms;
        item_out->producer_to_found_monotonic_ms =
            view.producer_to_found_monotonic_ms;
        item_out->wait_attempts = view.wait_attempts;
        item_out->activate_ms = view.activate_ms;
        item_out->metadata_ms = view.metadata_ms;
        return 0;
    }
    if (view.payload_kind != MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT) {
        printf("[mem_service] gap qwen3_scheduler_work_item=input_kind_invalid local=node%u kind=%u bytes=%" PRIu64 "\n",
               local_node + 1U,
               view.payload_kind,
               view.len);
        return -1;
    }
    item_out->kind = MEM_SERVICE_SCHEDULER_WORK_ITEM_RANGE_FORWARD;
    item_out->range_input = view;
    return 0;
}

int mem_service_obmm_service_v0_wait_runtime_range_input(uint32_t local_node,
                                                   uint32_t cluster_node_count,
                                                   uint64_t decode_step,
                                                   uint8_t *payload_out,
                                                   uint64_t payload_len,
                                                   uint64_t *checksum_out)
{
    struct mem_service_object_payload_view view;
    uint64_t hidden_range_bytes = mem_service_qwen3_handoff_hidden_bytes(decode_step);

    if (!payload_out || payload_len != hidden_range_bytes || !checksum_out) {
        return -1;
    }
    if (mem_service_obmm_service_v0_wait_runtime_range_input_view(local_node,
                                                            cluster_node_count,
                                                            decode_step,
                                                            &view) != 0 ||
        !view.data ||
        view.len != payload_len) {
        return -1;
    }
    memcpy(payload_out, view.data, payload_len);
    *checksum_out = view.checksum;
    return 0;
}
