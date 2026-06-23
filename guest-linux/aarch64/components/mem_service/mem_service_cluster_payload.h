#ifndef MEM_SERVICE_CLUSTER_PAYLOAD_H
#define MEM_SERVICE_CLUSTER_PAYLOAD_H

#include <stdint.h>

#include "mem_service.h"
#include "mem_service_cluster_payload_contract.h"
#include "mem_service_guest_runtime.h"

void mem_service_build_compact_summary(
    const struct mem_service_record *records,
    uint16_t record_count,
    struct mem_service_cluster_payload_compact_summary *summary);
int mem_service_write_cluster_payload(struct mem_service_cluster_runtime *rt,
                                      struct mem_service *svc,
                                      struct mem_service_cluster_slot *slot);

#endif
