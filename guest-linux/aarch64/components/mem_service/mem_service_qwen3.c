#include "mem_service_qwen3.h"

#include <limits.h>
#include <stddef.h>

#include "components/llm_infer/llm_infer.h"
#include "mem_service_object_contract.h"
#include "mem_service_profile.h"
#include "mem_service_qwen3_records.h"
#include "mem_service_qwen3_runtime.h"

uint32_t mem_service_qwen3_layer_count(void)
{
    uint64_t value = llm_infer_qwen3_total_layers();

    return value > UINT32_MAX ? 28U : (uint32_t)value;
}

uint32_t mem_service_qwen3_range_nodes(void)
{
    uint64_t value = llm_infer_qwen3_pipeline_nodes();

    return value == 0 || value > UINT32_MAX ? 8U : (uint32_t)value;
}

uint64_t mem_service_qwen3_hidden_range_bytes(void)
{
    return llm_infer_qwen3_hidden_range_bytes();
}

uint64_t mem_service_qwen3_handoff_hidden_bytes(uint64_t decode_step)
{
    return llm_infer_qwen3_handoff_hidden_bytes(decode_step);
}

const char *mem_service_qwen3_model_key(void)
{
    return llm_infer_qwen3_model_key();
}

uint64_t mem_service_qwen3_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count)
{
    uint64_t bytes_per_token =
        llm_infer_qwen3_range_kv_state_bytes(layer_start, layer_end);

    if (bytes_per_token == 0) {
        return 0;
    }
    return bytes_per_token * token_count;
}

int mem_service_qwen3_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out)
{
    if (cluster_node_count != mem_service_qwen3_range_nodes()) {
        return -1;
    }
    return llm_infer_qwen3_layer_range_for_node(local_node,
                                                cluster_node_count,
                                                layer_start_out,
                                                layer_end_out,
                                                next_node_out);
}

void mem_service_qwen3_node_range(uint32_t node,
                                  uint32_t node_count,
                                  uint32_t *start_out,
                                  uint32_t *end_out)
{
    uint32_t next_node = 0;

    if (!start_out || !end_out ||
        mem_service_qwen3_layer_range_for_node(node,
                                               node_count,
                                               start_out,
                                               end_out,
                                               &next_node) != 0) {
        if (start_out) {
            *start_out = 0;
        }
        if (end_out) {
            *end_out = 0;
        }
    }
}

/*
 * Placement-service wrappers. The qwen3 placement functions use
 * struct mem_service_qwen3_layer_range_placement, which is layout-identical
 * to the model-neutral struct mem_service_layer_range_placement declared in
 * mem_service_profile.h. Cast between them so the profile callbacks expose
 * the neutral type.
 */
static int qwen3_publish_layer_range_placements(struct mem_service *svc,
                                                uint32_t node_count)
{
    return mem_service_publish_qwen3_layer_range_placements(svc, node_count);
}

static bool qwen3_read_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    return mem_service_read_qwen3_layer_range_placement(
        svc,
        owner_node,
        (struct mem_service_qwen3_layer_range_placement *)out);
}

static bool qwen3_find_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    return mem_service_find_qwen3_layer_range_predecessor(
        svc,
        owner_node,
        (struct mem_service_qwen3_layer_range_placement *)out);
}

/*
 * qwen3 model profile. Bundles the existing geometry/kind surface into a
 * const struct so the core can route through mem_service_active_model_profile()
 * without naming qwen3. Values match the current constants exactly.
 */
const struct mem_service_model_profile *mem_service_qwen3_profile(void)
{
    static const struct mem_service_model_profile profile = {
        .name = "qwen3",
        .key_namespace = "qwen3",
        .layer_count = mem_service_qwen3_layer_count,
        .range_nodes = mem_service_qwen3_range_nodes,
        .hidden_range_bytes = mem_service_qwen3_hidden_range_bytes,
        .handoff_hidden_bytes = mem_service_qwen3_handoff_hidden_bytes,
        .range_kv_state_bytes = mem_service_qwen3_range_kv_state_bytes,
        .model_key = mem_service_qwen3_model_key,
        .layer_range_for_node = mem_service_qwen3_layer_range_for_node,
        .obmm_kind_token_result = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
        .obmm_token_result_bytes = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
        .obmm_kind_kv_state = MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
        .obmm_kind_engram_history = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
        .obmm_kind_engram_candidates = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
        .obmm_kind_engram_selected = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
        .obmm_kind_engram_state = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
        .recycle_runtime_record = mem_service_recycle_qwen3_runtime_record,
        .publish_layer_range_placements = qwen3_publish_layer_range_placements,
        .read_layer_range_placement = qwen3_read_layer_range_placement,
        .find_layer_range_predecessor = qwen3_find_layer_range_predecessor,
    };
    return &profile;
}
