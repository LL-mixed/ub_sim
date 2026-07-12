#ifndef MEM_SERVICE_CLUSTER_RUNTIME_H
#define MEM_SERVICE_CLUSTER_RUNTIME_H

#include "mem_service_gsva_access.h"
#include "mem_service_guest_runtime.h"

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void);
int mem_service_cluster_runtime_init(struct mem_service_cluster_runtime *rt);
int mem_service_cluster_runtime_require(struct mem_service_cluster_runtime *rt);
int mem_service_cluster_runtime_pipeline_start_barrier(
    struct mem_service_cluster_runtime *rt,
    uint64_t timeout_ms);
void mem_service_cluster_runtime_destroy(struct mem_service_cluster_runtime *rt);
int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);
int mem_service_refresh_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);
int mem_service_cluster_runtime_make_gsva_buffer_desc(
    const struct mem_service_cluster_runtime *rt,
    const struct mem_service_record *record,
    struct mem_service_gsva_buffer_desc *out);

/*
 * OBMM pool layout/usage reporting. Declared in the core runtime header
 * because the cluster runtime owns the pool; the implementation currently
 * lives in the qwen3 runtime translation unit but is model-neutral.
 */
void mem_service_report_obmm_pool_layout_once(struct mem_service_cluster_runtime *rt);
void mem_service_report_obmm_pool_usage(struct mem_service_cluster_runtime *rt,
                                        uint32_t local_node,
                                        uint64_t decode_step);

#endif
