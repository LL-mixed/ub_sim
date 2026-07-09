#ifndef MEM_SERVICE_CLUSTER_QUEUE_H
#define MEM_SERVICE_CLUSTER_QUEUE_H

#include "mem_service.h"
#include "mem_service_guest_runtime.h"

#include <stdbool.h>
#include <stdint.h>

bool mem_service_token_result_desc_matches(const struct obmm_desc *desc,
                                           uint16_t epoch,
                                           uint32_t payload_kind,
                                           uint64_t payload_len);
bool mem_service_object_desc_matches(const struct obmm_desc *desc,
                                     uint16_t epoch,
                                     uint32_t payload_kind,
                                     uint64_t min_payload_len,
                                     uint64_t max_payload_len);
bool mem_service_object_desc_kind_len_matches(const struct obmm_desc *desc,
                                              uint32_t payload_kind,
                                              uint64_t min_payload_len,
                                              uint64_t max_payload_len);
bool mem_service_runtime_range_input_desc_matches(const struct obmm_desc *desc,
                                                  uint64_t expected_payload_len,
                                                  uint16_t epoch);
bool mem_service_take_pending_runtime_range_input_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint64_t expected_payload_len,
    uint16_t epoch,
    struct obmm_desc *desc_out);
bool mem_service_take_pending_token_result_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    uint32_t payload_kind,
    uint64_t payload_len,
    struct obmm_desc *desc_out);
bool mem_service_take_pending_object_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    uint32_t payload_kind,
    uint64_t min_payload_len,
    uint64_t max_payload_len,
    struct obmm_desc *desc_out);
bool mem_service_take_pending_object_kind_len_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint32_t payload_kind,
    uint64_t min_payload_len,
    uint64_t max_payload_len,
    struct obmm_desc *desc_out);
void mem_service_stash_pending_desc(struct mem_service_cluster_runtime *rt,
                                    int owner_idx,
                                    const struct obmm_desc *desc);
int mem_service_queue_barrier(struct mem_service_cluster_runtime *rt,
                              uint16_t desc_type,
                              uint16_t epoch,
                              uint32_t publish_seq);
int mem_service_push_obmm_object_descs(struct mem_service_cluster_runtime *rt,
                                       uint32_t payload_kind,
                                       uint64_t payload_offset,
                                       uint64_t payload_len,
                                       uint64_t checksum,
                                       uint16_t epoch);
int mem_service_push_obmm_object_desc_to(struct mem_service_cluster_runtime *rt,
                                         uint32_t target_node,
                                         uint32_t payload_kind,
                                         uint64_t payload_offset,
                                         uint64_t payload_len,
                                         uint64_t checksum,
                                         uint16_t epoch);
int mem_service_wait_remote_obmm_object_descs(struct mem_service_cluster_runtime *rt,
                                             uint32_t owner_node,
                                             uint16_t epoch,
                                             const struct mem_service_record *weight,
                                             const struct mem_service_record *kvcache,
                                             const struct mem_service_record *hidden_input,
                                             const struct mem_service_record *hidden_output);

#endif
