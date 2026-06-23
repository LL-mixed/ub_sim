#include "mem_service_internal.h"
#include "mem_service_cluster_payload.h"
#include "mem_service_cluster_observe.h"
#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_read.h"
#include "mem_service_cluster_runtime.h"
#include "mem_service_cluster_utils.h"
#include "mem_service_obmm_object_flow.h"
#include "mem_service_obmm_objects.h"
#include "mem_service_object_refs.h"
#include "mem_service_qwen3_records.h"
#include "mem_service_qwen3_runtime.h"
#include "mem_service_record_table.h"

static struct mem_service_cluster_runtime g_mem_service_cluster_runtime;

struct mem_service_cluster_runtime *mem_service_cluster_runtime_current(void)
{
    return &g_mem_service_cluster_runtime;
}

#include "mem_service_qwen3_runtime.inc"

#include "mem_service_qwen3_runtime_range_wait_flow.inc"

#include "mem_service_qwen3_runtime_range_publish_flow.inc"

#include "mem_service_qwen3_kv_state_flow.inc"

#include "mem_service_qwen3_terminal_token_flow.inc"

#include "mem_service_qwen3_engram_publish_flow.inc"

#include "mem_service_qwen3_engram_wait_flow.inc"
