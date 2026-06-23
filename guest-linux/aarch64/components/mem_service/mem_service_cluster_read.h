#ifndef MEM_SERVICE_CLUSTER_READ_H
#define MEM_SERVICE_CLUSTER_READ_H

#include "mem_service.h"
#include "mem_service_cluster_payload_contract.h"
#include "mem_service_guest_runtime.h"

#include <stdbool.h>
#include <stdint.h>

bool mem_service_try_read_stable_payload_region(
    const struct mem_service_cluster_slot *slot,
    struct mem_service_cluster_payload *snapshot,
    struct mem_service_cluster_payload_header *seen_out);

bool mem_service_try_read_stable_compact_summary_region(
    const struct mem_service_cluster_slot *slot,
    struct mem_service_cluster_payload_compact_summary *summary,
    struct mem_service_cluster_payload_header *seen_out);

bool mem_service_wait_compact_summary_region_at_least(
    const struct mem_service_cluster_slot *slot,
    uint32_t min_publish_done_seq,
    long timeout_ms,
    struct mem_service_cluster_payload_compact_summary *summary,
    struct mem_service_cluster_payload_header *seen_out);

bool mem_service_slot_find_record(
    const struct mem_service_cluster_slot *slot,
    const char *key,
    struct mem_service_record *resolved_out);

bool mem_service_slot_find_record_by_obmm_object_backing(
    const struct mem_service_cluster_slot *slot,
    enum mem_service_record_kind record_kind,
    uint32_t payload_kind,
    uint64_t payload_offset,
    uint64_t payload_len,
    uint32_t checksum_cookie,
    struct mem_service_record *resolved_out);

#endif
