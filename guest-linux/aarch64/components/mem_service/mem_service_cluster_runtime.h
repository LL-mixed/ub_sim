#ifndef MEM_SERVICE_CLUSTER_RUNTIME_H
#define MEM_SERVICE_CLUSTER_RUNTIME_H

#include "mem_service_guest_runtime.h"

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void);
int mem_service_cluster_runtime_init(struct mem_service_cluster_runtime *rt);
int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt);
void mem_service_cluster_runtime_destroy(struct mem_service_cluster_runtime *rt);
int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);

#endif
