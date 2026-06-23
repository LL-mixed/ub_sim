#ifndef MEM_SERVICE_CLUSTER_OBSERVE_H
#define MEM_SERVICE_CLUSTER_OBSERVE_H

#include "mem_service.h"

#include <stdint.h>

int mem_service_cluster_fetch_record(struct mem_service *svc,
                                     const char *key,
                                     struct mem_service_record *resolved_out);
int mem_service_obmm_service_v0_ensure_cluster_runtime(uint32_t local_node,
                                                       uint32_t cluster_node_count);
int mem_service_publish_observe_cluster(struct mem_service *svc,
                                        const struct mem_service_record *local_record,
                                        struct mem_service_cluster_summary *summary);

#endif
