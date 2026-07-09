#ifndef MEM_SERVICE_EXPERT_ROUTE_FLOW_H
#define MEM_SERVICE_EXPERT_ROUTE_FLOW_H

/*
 * DeepSeek V4 Flash expert route flow (stage 2).
 *
 * Per the plan (section 3.2), MoE routing is a layer-internal concern: each
 * layer produces a routing decision (which 6 of 256 routed experts are
 * active for this token) that is consumed within the same layer. It is an
 * attachment to range_publish_flow output, NOT a new cross-layer handoff
 * flow. The cross-layer interface (hidden range + KV state) is unchanged.
 *
 * This header declares the route-decision record helpers used by the
 * per-layer forward path to record which experts were activated, and the
 * weight-tile addressing used to fetch expert payloads from the object
 * store on demand (plan section 3.3).
 */

#include <stdint.h>

#include "mem_service.h"

/* Maximum active routed experts recorded per (layer, token) decision. */
#define MEM_SERVICE_EXPERT_ROUTE_TOP_K 6U
#define MEM_SERVICE_EXPERT_MAX_EXPERTS 256U
#define MEM_SERVICE_EXPERT_WEIGHT_TILE_DEFAULT_BYTES (2048ULL * 1024ULL)

/* Quant tags for the ds4 mixed-precision recipe (plan section 3.3). */
#define MEM_SERVICE_EXPERT_QUANT_IQ2_XXS "iq2_xxs"
#define MEM_SERVICE_EXPERT_QUANT_Q2_K "q2_k"

/*
 * One routing decision for one token at one layer: the active routed
 * expert ids. Mirrors Rust ExpertRouteDecision.
 */
struct mem_service_expert_route_decision {
    uint64_t step_index;
    uint32_t layer_id;
    uint32_t token_index;
    uint32_t active_expert_count;
    uint32_t active_experts[MEM_SERVICE_EXPERT_ROUTE_TOP_K];
};

struct mem_service_expert_weight_tile_ref {
    char model_key[64];
    uint32_t layer_id;
    uint32_t expert_id;
    char quant[16];
    char object_key[160];
    uint64_t payload_bytes;
    uint64_t payload_checksum;
};

int mem_service_expert_route_decision_init(
    struct mem_service_expert_route_decision *decision,
    uint64_t step_index,
    uint32_t layer_id,
    uint32_t token_index,
    const uint32_t *active_experts,
    uint32_t active_expert_count);

int mem_service_expert_route_decision_for_decode(
    struct mem_service_expert_route_decision *decision,
    uint64_t step_index,
    uint32_t layer_id,
    uint32_t token_index,
    uint32_t expert_count);

/*
 * Build the object-store key for one expert weight tile. Addressing is
 * (model, layer, expert_id, quant) per plan section 3.3.
 *
 * Returns 0 on success, -1 if the buffer is too small.
 */
int mem_service_expert_weight_tile_key(char *out,
                                       size_t out_len,
                                       const char *model_key,
                                       uint32_t layer_id,
                                       uint32_t expert_id,
                                       const char *quant);

int mem_service_expert_weight_tile_ref_init(
    struct mem_service_expert_weight_tile_ref *ref,
    const char *model_key,
    uint32_t layer_id,
    uint32_t expert_id,
    const char *quant,
    uint64_t payload_bytes);

/*
 * Resolve one expert weight tile from a complete Flash weight catalog file.
 * The catalog is produced by `sim-cli deepseek-v4-flash-weight-catalog` and
 * carries the provider payload size/checksum for every (layer, expert).
 *
 * Returns 0 on success, -1 on parse/coverage/model mismatch. A configured
 * catalog is authoritative: callers should fail closed instead of falling
 * back to deterministic defaults when this returns -1.
 */
int mem_service_expert_weight_tile_ref_from_catalog_file(
    struct mem_service_expert_weight_tile_ref *ref,
    const char *catalog_path,
    const char *model_key,
    uint32_t layer_id,
    uint32_t expert_id);

/*
 * Record one routing decision into a stable key for the object store.
 * The decision is layer-internal; this produces a record key like
 * "route/<model>/step<S>/layer<L>/token<T>" for audit/logging.
 */
int mem_service_expert_route_record_key(char *out,
                                        size_t out_len,
                                        const char *model_key,
                                        uint64_t step_index,
                                        uint32_t layer_id,
                                        uint32_t token_index);

#endif
