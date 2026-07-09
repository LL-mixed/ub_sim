#ifndef MEM_SERVICE_OBMM_OBJECTS_H
#define MEM_SERVICE_OBMM_OBJECTS_H

#include "mem_service.h"
#include "mem_service_profile.h"

#include <stdint.h>

struct mem_service_cluster_runtime;

void mem_service_fill_obmm_object_payload(uint8_t *dst,
                                          uint64_t len,
                                          uint32_t owner_node,
                                          uint32_t payload_kind);
const char *mem_service_object_kind_name(uint32_t payload_kind);
int mem_service_payload_arena_alloc(struct mem_service_cluster_runtime *rt,
                                    uint64_t bytes,
                                    uint64_t align,
                                    uint64_t *offset_out);
int mem_service_put_obmm_object_record(struct mem_service *svc,
                                       mem_service_record_recycler_fn recycle_runtime_record,
                                       enum mem_service_record_kind record_kind,
                                       const char *key,
                                       uint32_t owner_node,
                                       uint32_t payload_kind,
                                       uint64_t offset,
                                       uint64_t len,
                                       uint64_t checksum,
                                       struct mem_service_record *resolved_out);

#endif
