#include "mem_service_internal.h"

#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3.h"
#include "mem_service_qwen3_runtime.h"

int mem_service_publish_decode_round_done(struct mem_service *svc,
                                          uint32_t local_node,
                                          uint32_t cluster_node_count,
                                          uint64_t decode_step,
                                          uint64_t round_scope_hash)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    uint64_t payload_words[8];
    uint64_t checksum;
    uint64_t slot_index;
    uint64_t slot_offset;
    uint8_t *base;

    if (!svc || cluster_node_count == 0U || cluster_node_count > 31U ||
        local_node >= cluster_node_count ||
        mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (cluster_node_count != (uint32_t)rt->node_count) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if ((uint32_t)rt->local_idx != local_node || !local_slot->region.addr ||
        local_slot->region.len <
            MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_REGION_BYTES) {
        return -1;
    }

    slot_index = (decode_step ^
                  (round_scope_hash * 11400714819323198485ULL)) &
                 (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS - 1ULL);
    slot_offset = MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                  slot_index * MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES;
    payload_words[0] = 0x71336465636f6465ULL;
    payload_words[1] = decode_step;
    payload_words[2] = local_node;
    payload_words[3] = cluster_node_count;
    payload_words[4] = rt->publish_seq;
    payload_words[5] = rt->observe_epoch;
    payload_words[6] = round_scope_hash;
    payload_words[7] = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                            7U * sizeof(payload_words[0]));
    checksum = mem_service_checksum_bytes((const uint8_t *)payload_words,
                                    MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);

    base = (uint8_t *)local_slot->region.addr;
    memcpy(base + slot_offset, payload_words, MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);
    if (mem_service_update_region_range_at(local_slot, slot_offset,
                                     MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES, true) != 0) {
        return -1;
    }
    (void)msync(base + slot_offset, MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES, MS_SYNC);
    printf("[mem_service] stage decode_round_done_publish local=node%u step=%" PRIu64
           " offset=0x%016" PRIx64 " slot=%" PRIu64
           " bytes=%" PRIu64 " scope_hash=0x%016" PRIx64
           " checksum=0x%016" PRIx64
           " backing=obmm_pool status=ok\n",
           local_node + 1U,
           decode_step,
           slot_offset,
           slot_index,
           (uint64_t)MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES,
           round_scope_hash,
           checksum);
    mem_service_report_obmm_pool_usage(rt, local_node, decode_step);
    return 0;
}

int mem_service_wait_all_decode_round_done(struct mem_service *svc,
                                           uint32_t cluster_node_count,
                                           uint64_t decode_step,
                                           uint64_t round_scope_hash,
                                           uint64_t timeout_ms)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    long deadline;
    uint32_t ready_mask = 0;
    uint32_t expected_mask;
    uint64_t slot_index;
    uint64_t slot_offset;

    if (!svc || cluster_node_count == 0U || cluster_node_count > 31U ||
        mem_service_cluster_runtime_require(rt) != 0) {
        return -1;
    }
    if (cluster_node_count != (uint32_t)rt->node_count) {
        return -1;
    }
    slot_index = (decode_step ^
                  (round_scope_hash * 11400714819323198485ULL)) &
                 (MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_SLOTS - 1ULL);
    slot_offset = MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_OFFSET +
                  slot_index * MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES;
    expected_mask = (1U << cluster_node_count) - 1U;
    deadline = obmm_now_ms() + (long)timeout_ms;
    while (obmm_now_ms() < deadline) {
        ready_mask = 0;
        for (uint32_t i = 0; i < cluster_node_count; ++i) {
            struct mem_service_cluster_slot *slot;
            uint64_t payload_words[8];

            if ((int)i != rt->local_idx &&
                mem_service_activate_remote_slot(rt, (int)i) != 0) {
                continue;
            }
            slot = &rt->slots[i];
            if (!slot->region.addr ||
                slot->region.len <
                    slot_offset + MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES) {
                continue;
            }
            memcpy(payload_words,
                   (uint8_t *)slot->region.addr + slot_offset,
                   MEM_SERVICE_OBMM_QWEN3_ROUND_DONE_BYTES);
            if (payload_words[0] == 0x71336465636f6465ULL &&
                payload_words[1] == decode_step &&
                payload_words[2] == i &&
                payload_words[3] == cluster_node_count &&
                payload_words[6] == round_scope_hash &&
                payload_words[7] ==
                    mem_service_checksum_bytes((const uint8_t *)payload_words,
                                         7U * sizeof(payload_words[0]))) {
                ready_mask |= 1U << i;
            }
        }
        if (ready_mask == expected_mask) {
            printf("[mem_service] stage decode_round_barrier step=%" PRIu64
                   " nodes=%u ready_mask=0x%02x slot=%" PRIu64
                   " scope_hash=0x%016" PRIx64 " status=ok\n",
                   decode_step,
                   cluster_node_count,
                   ready_mask,
                   slot_index,
                   round_scope_hash);
            return 0;
        }
        usleep(10000);
    }
    printf("[mem_service] gap decode_round_barrier=timeout step=%" PRIu64
           " nodes=%u ready_mask=0x%02x expected_mask=0x%02x"
           " slot=%" PRIu64 " scope_hash=0x%016" PRIx64 "\n",
           decode_step,
           cluster_node_count,
           ready_mask,
           expected_mask,
           slot_index,
           round_scope_hash);
    return -1;
}
