#include "mem_service_cluster_payload.h"

#include "mem_service_cluster_utils.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static uint16_t mem_service_snapshot_metadata_records(struct mem_service *svc,
                                                      struct mem_service_record *out,
                                                      uint16_t max_records)
{
    uint16_t count = 0;
    size_t i;

    if (!svc || !out || max_records == 0) {
        return 0;
    }
    for (i = 0; i < MEM_SERVICE_MAX_RECORDS && count < max_records; ++i) {
        if (!svc->records[i].in_use) {
            continue;
        }
        out[count++] = svc->records[i];
    }
    return count;
}

void mem_service_build_compact_summary(
    const struct mem_service_record *records,
    uint16_t record_count,
    struct mem_service_cluster_payload_compact_summary *summary)
{
    uint16_t i;

    memset(summary, 0, sizeof(*summary));
    summary->record_count = record_count;
    summary->flags = MEM_SERVICE_COMPACT_PREFIX_STATE_READY | MEM_SERVICE_COMPACT_PREFIX_VIEW_READY;
    for (i = 0; i < record_count; ++i) {
        const struct mem_service_record *rec = &records[i];

        if (!rec->in_use) {
            summary->flags &= (uint16_t)~(MEM_SERVICE_COMPACT_PREFIX_STATE_READY |
                                          MEM_SERVICE_COMPACT_PREFIX_VIEW_READY);
            continue;
        }
        if (rec->kind == MEM_SERVICE_RECORD_REQUEST_PREFIX) {
            summary->prefix_count += 1;
            if (summary->prefix_version_floor == 0 ||
                rec->version < summary->prefix_version_floor) {
                summary->prefix_version_floor = rec->version;
            }
            if (summary->prefix_result_floor == 0 ||
                rec->last_result_segment < summary->prefix_result_floor) {
                summary->prefix_result_floor = rec->last_result_segment;
            }
            if (rec->state != MEM_SERVICE_KVCACHE_STATE_RELOADED) {
                summary->flags &= (uint16_t)~MEM_SERVICE_COMPACT_PREFIX_STATE_READY;
            }
            if (rec->hot_segment_id == 0 || rec->last_result_segment == 0) {
                summary->flags &= (uint16_t)~MEM_SERVICE_COMPACT_PREFIX_VIEW_READY;
            }
        } else if (rec->kind == MEM_SERVICE_RECORD_PREFIX_GROUP) {
            summary->group_count += 1;
        } else if (rec->kind == MEM_SERVICE_RECORD_BLOCK_META) {
            summary->block_count += 1;
            if (summary->block_version_floor == 0 ||
                rec->version < summary->block_version_floor) {
                summary->block_version_floor = rec->version;
            }
            if (summary->block_result_floor == 0 ||
                rec->last_result_segment < summary->block_result_floor) {
                summary->block_result_floor = rec->last_result_segment;
            }
        } else if (rec->kind == MEM_SERVICE_RECORD_WEIGHT_TILE) {
            summary->weight_tile_count += 1;
        } else if (rec->kind == MEM_SERVICE_RECORD_KVCACHE_OBJECT) {
            summary->kvcache_object_count += 1;
        } else if (rec->kind == MEM_SERVICE_RECORD_HIDDEN_RANGE_INPUT ||
                   rec->kind == MEM_SERVICE_RECORD_HIDDEN_RANGE_OUTPUT) {
            summary->hidden_range_count += 1;
        } else if (rec->kind == MEM_SERVICE_RECORD_LAYER_RANGE_PLACEMENT) {
            summary->hidden_range_count += 1;
        } else if (rec->kind == MEM_SERVICE_RECORD_RUNTIME_HANDOFF ||
                   rec->kind == MEM_SERVICE_RECORD_EXECUTION_ARTIFACT ||
                   rec->kind == MEM_SERVICE_RECORD_TRAINING_ARTIFACT) {
            continue;
        } else {
            summary->flags &= (uint16_t)~(MEM_SERVICE_COMPACT_PREFIX_STATE_READY |
                                          MEM_SERVICE_COMPACT_PREFIX_VIEW_READY);
        }
    }
}

int mem_service_write_cluster_payload(struct mem_service_cluster_runtime *rt,
                                      struct mem_service *svc,
                                      struct mem_service_cluster_slot *slot)
{
    struct mem_service_cluster_payload *payload;
    struct mem_service_cluster_payload_compact_summary compact;
    uint32_t seq;
    int rc = -1;

    if (!rt || !svc || !slot || !slot->region.addr) {
        return -1;
    }
    payload = calloc(1, sizeof(*payload));
    if (!payload) {
        return -1;
    }
    seq = ++rt->publish_seq;
    if (seq == 0) {
        seq = ++rt->publish_seq;
    }
    payload->magic = MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC;
    payload->version = MEM_SERVICE_CLUSTER_PAYLOAD_VERSION;
    payload->record_count = mem_service_snapshot_metadata_records(svc,
                                                                  payload->records,
                                                                  MEM_SERVICE_CLUSTER_MAX_RECORDS);
    mem_service_build_compact_summary(payload->records, payload->record_count, &compact);
    memcpy(payload->record_pad, &compact, sizeof(compact));
    payload->publish_seq = seq;
    payload->publish_done_seq = 0;
    memset(slot->region.addr, 0, sizeof(*payload));
    memcpy(slot->region.addr, payload, sizeof(*payload));
    __sync_synchronize();
    ((struct mem_service_cluster_payload *)slot->region.addr)->publish_done_seq = seq;
    __sync_synchronize();
    if (mem_service_update_region_range(slot, true) != 0) {
        goto out;
    }
    (void)msync(slot->region.addr, sizeof(*payload), MS_SYNC);
    {
        const uint8_t *bytes = (const uint8_t *)slot->region.addr;
        uint64_t probe_040 = 0;
        uint64_t probe_048 = 0;
        uint64_t probe_050 = 0;

        memcpy(&probe_040, bytes + 0x40, sizeof(probe_040));
        memcpy(&probe_048, bytes + 0x48, sizeof(probe_048));
        memcpy(&probe_050, bytes + 0x50, sizeof(probe_050));
        printf("[mem_service] stage db_service_cluster_debug owner=node%d step=write_local_payload probe040=%#" PRIx64 " probe048=%#" PRIx64 " probe050=%#" PRIx64 "\n",
               slot->owner_idx + 1,
               probe_040,
               probe_048,
               probe_050);
    }
    printf("[mem_service] stage db_service_cluster_debug owner=node%d step=write_local_done seq=%u done=%u count=%u\n",
           slot->owner_idx + 1,
           ((const struct mem_service_cluster_payload *)slot->region.addr)->publish_seq,
           ((const struct mem_service_cluster_payload *)slot->region.addr)->publish_done_seq,
           ((const struct mem_service_cluster_payload *)slot->region.addr)->record_count);
    rc = 0;

out:
    free(payload);
    return rc;
}
