#include "mem_service_deepseek_v4_flash.h"

#include <limits.h>
#include <stddef.h>

#include "mem_service_object_contract.h"
#include "mem_service_qwen3_records.h"
#include "mem_service_qwen3_runtime.h"

/*
 * DeepSeek V4 Flash geometry constants. Mirror ds4 DS4_SHAPE_FLASH
 * (ds4.c:177-212). Stage 1 hardcodes them; stage 2 may make them env-driven
 * like llm_infer_qwen3_* once the MoE path lands.
 */
#define DEEPSEEK_V4_FLASH_TOTAL_LAYERS 43U
#define DEEPSEEK_V4_FLASH_PIPELINE_NODES 8U
#define DEEPSEEK_V4_FLASH_HIDDEN_SIZE 4096ULL
#define DEEPSEEK_V4_FLASH_PREFILL_TOKENS 128ULL
#define DEEPSEEK_V4_FLASH_DECODE_TOKENS 1ULL
#define DEEPSEEK_V4_FLASH_KV_HEADS 1ULL
#define DEEPSEEK_V4_FLASH_HEAD_DIM 512ULL
#define DEEPSEEK_V4_FLASH_KV_STREAMS 2ULL
#define DEEPSEEK_V4_FLASH_KV_ELEM_BYTES 4ULL
#define DEEPSEEK_V4_FLASH_MODEL_KEY "deepseek-v4-flash"

static uint32_t flash_layer_count(void)
{
    return DEEPSEEK_V4_FLASH_TOTAL_LAYERS;
}

static uint32_t flash_range_nodes(void)
{
    return DEEPSEEK_V4_FLASH_PIPELINE_NODES;
}

/*
 * step0 handoff carries the full prefill hidden range; step>0 carries the
 * smaller decode-hidden shape. Matches the qwen3 handoff convention
 * (llm_infer.c:90-95) but with Flash hidden size.
 */
static uint64_t flash_hidden_range_bytes(void)
{
    return DEEPSEEK_V4_FLASH_HIDDEN_SIZE * DEEPSEEK_V4_FLASH_PREFILL_TOKENS * 2ULL;
}

static uint64_t flash_decode_hidden_bytes(void)
{
    return DEEPSEEK_V4_FLASH_HIDDEN_SIZE * DEEPSEEK_V4_FLASH_DECODE_TOKENS * 2ULL;
}

static uint64_t flash_handoff_hidden_bytes(uint64_t decode_step)
{
    return decode_step > 0 ? flash_decode_hidden_bytes() : flash_hidden_range_bytes();
}

static const char *flash_model_key(void)
{
    return DEEPSEEK_V4_FLASH_MODEL_KEY;
}

/*
 * KV state bytes per layer range. Flash uses compressed sparse attention
 * (ratio-4 even layers, ratio-128 odd layers from layer 2 onward, raw
 * sliding window for layers 0-1). Stage 1 uses the same per-layer constant
 * model as qwen3 (plan section 3.4: per-layer budget, not per-token growth);
 * the coefficient is kv_heads * head_dim * kv_streams * kv_elem_bytes.
 * Stage 2 will refine this per layer type.
 */
static uint64_t flash_range_kv_state_bytes(uint32_t layer_start,
                                           uint32_t layer_end,
                                           uint64_t token_count)
{
    uint64_t bytes_per_token_per_layer;
    uint64_t layer_count;

    if (layer_end <= layer_start || layer_end > DEEPSEEK_V4_FLASH_TOTAL_LAYERS) {
        return 0;
    }
    bytes_per_token_per_layer = DEEPSEEK_V4_FLASH_KV_HEADS *
                                DEEPSEEK_V4_FLASH_HEAD_DIM *
                                DEEPSEEK_V4_FLASH_KV_STREAMS *
                                DEEPSEEK_V4_FLASH_KV_ELEM_BYTES;
    layer_count = (uint64_t)(layer_end - layer_start);
    return layer_count * bytes_per_token_per_layer * token_count;
}

static int flash_layer_range_for_node(uint32_t local_node,
                                      uint32_t cluster_node_count,
                                      uint32_t *layer_start_out,
                                      uint32_t *layer_end_out,
                                      uint32_t *next_node_out)
{
    uint32_t layer_count = DEEPSEEK_V4_FLASH_TOTAL_LAYERS;
    uint32_t base;
    uint32_t rem;
    uint32_t idx;
    uint32_t start;
    uint32_t count;

    if (cluster_node_count != DEEPSEEK_V4_FLASH_PIPELINE_NODES ||
        local_node >= cluster_node_count ||
        !layer_start_out || !layer_end_out || !next_node_out) {
        return -1;
    }
    base = layer_count / cluster_node_count;
    rem = layer_count % cluster_node_count;
    idx = local_node;
    start = idx * base + (idx < rem ? idx : rem);
    count = base + (idx < rem ? 1U : 0U);
    *layer_start_out = start;
    *layer_end_out = start + count;
    *next_node_out = (local_node + 1U) % cluster_node_count;
    return 0;
}

/*
 * Placement service: reuse the qwen3 placement record mechanism. The
 * placement struct is layout-identical (model-neutral) and the publish/read
 * functions store placement metadata keyed by model, so Flash gets its own
 * namespace via the model key. Wrappers cast between the qwen3 struct name
 * and the neutral struct.
 */
static int flash_publish_layer_range_placements(struct mem_service *svc,
                                                uint32_t node_count)
{
    return mem_service_publish_qwen3_layer_range_placements(svc, node_count);
}

static bool flash_read_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    return mem_service_read_qwen3_layer_range_placement(
        svc,
        owner_node,
        (struct mem_service_qwen3_layer_range_placement *)out);
}

static bool flash_find_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    return mem_service_find_qwen3_layer_range_predecessor(
        svc,
        owner_node,
        (struct mem_service_qwen3_layer_range_placement *)out);
}

const struct mem_service_model_profile *mem_service_deepseek_v4_flash_profile(void)
{
    static const struct mem_service_model_profile profile = {
        .name = "deepseek-v4-flash",
        .key_namespace = "deepseek-v4-flash",
        .layer_count = flash_layer_count,
        .range_nodes = flash_range_nodes,
        .hidden_range_bytes = flash_hidden_range_bytes,
        .handoff_hidden_bytes = flash_handoff_hidden_bytes,
        .range_kv_state_bytes = flash_range_kv_state_bytes,
        .model_key = flash_model_key,
        .layer_range_for_node = flash_layer_range_for_node,
        /* Stage 1 reuses the shared OBMM object kinds (same layout). */
        .obmm_kind_token_result = MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT,
        .obmm_token_result_bytes = MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES,
        .obmm_kind_kv_state = MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE,
        .obmm_kind_engram_history = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_HISTORY,
        .obmm_kind_engram_candidates = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_CANDIDATES,
        .obmm_kind_engram_selected = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_SELECTED,
        .obmm_kind_engram_state = MEM_SERVICE_OBMM_KIND_QWEN3_ENGRAM_STATE,
        .recycle_runtime_record = mem_service_recycle_qwen3_runtime_record,
        .publish_layer_range_placements = flash_publish_layer_range_placements,
        .read_layer_range_placement = flash_read_layer_range_placement,
        .find_layer_range_predecessor = flash_find_layer_range_predecessor,
    };
    return &profile;
}
