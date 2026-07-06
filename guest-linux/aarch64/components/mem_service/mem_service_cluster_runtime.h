#ifndef MEM_SERVICE_CLUSTER_RUNTIME_H
#define MEM_SERVICE_CLUSTER_RUNTIME_H

#include "mem_service_guest_runtime.h"
#include "mem_service_ub_ssd_gsva_backend.h"

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void);
int mem_service_cluster_runtime_init(struct mem_service_cluster_runtime *rt);
int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt);
void mem_service_cluster_runtime_destroy(struct mem_service_cluster_runtime *rt);
int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);
int mem_service_refresh_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);
int mem_service_cluster_runtime_make_ub_ssd_gsva_buffer_desc(
    const struct mem_service_cluster_runtime *rt,
    const struct mem_service_record *record,
    struct mem_service_ub_ssd_gsva_buffer_desc *out);

#endif
