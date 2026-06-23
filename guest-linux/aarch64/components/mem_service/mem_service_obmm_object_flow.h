#ifndef MEM_SERVICE_OBMM_OBJECT_FLOW_H
#define MEM_SERVICE_OBMM_OBJECT_FLOW_H

#include "mem_service.h"

#include <stdint.h>

int mem_service_obmm_service_v0_publish_resolve(struct mem_service *svc,
                                                uint32_t local_node,
                                                uint32_t cluster_node_count);

#endif
