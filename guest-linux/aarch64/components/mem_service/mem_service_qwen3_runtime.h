#ifndef MEM_SERVICE_QWEN3_RUNTIME_H
#define MEM_SERVICE_QWEN3_RUNTIME_H

#include "mem_service_guest_runtime.h"

#include <stdint.h>

void mem_service_report_obmm_pool_usage(struct mem_service_cluster_runtime *rt,
                                        uint32_t local_node,
                                        uint64_t decode_step);

#endif
