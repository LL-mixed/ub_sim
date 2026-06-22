#include "mem_service_internal.h"
#include "mem_service_qwen3_records.h"
#include "mem_service_record_table.h"

static struct mem_service_cluster_runtime g_mem_service_cluster_runtime;

static int mem_service_activate_remote_slot(struct mem_service_cluster_runtime *rt, int owner_idx);

#include "mem_service_cluster_utils.inc"

#include "mem_service_cluster_payload.inc"

#include "mem_service_object_refs.inc"

#include "mem_service_obmm_objects.inc"

#include "mem_service_qwen3_runtime.inc"

#include "mem_service_cluster_read.inc"

#include "mem_service_cluster_runtime.inc"

#include "mem_service_cluster_queue.inc"

#include "mem_service_qwen3_records.inc"
#include "mem_service_keys.inc"
#include "mem_service_metadata.inc"

#include "mem_service_cluster_observe.inc"

#include "mem_service_obmm_object_flow.inc"

#include "mem_service_qwen3_runtime_range_wait_flow.inc"

#include "mem_service_qwen3_runtime_range_publish_flow.inc"

#include "mem_service_qwen3_kv_state_flow.inc"

#include "mem_service_qwen3_terminal_token_flow.inc"

#include "mem_service_qwen3_engram_publish_flow.inc"

#include "mem_service_qwen3_engram_wait_flow.inc"

#include "mem_service_qwen3_decode_barrier.inc"
