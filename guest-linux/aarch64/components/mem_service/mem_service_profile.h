#ifndef MEM_SERVICE_PROFILE_H
#define MEM_SERVICE_PROFILE_H

/*
 * mem_service_model_profile — model adapter interface.
 *
 * The mem_service core is model-neutral. Each model family supplies a
 * const profile that bundles geometry queries, an OBMM object-kind map, a
 * key namespace, and a layer-range placement descriptor. The core routes
 * geometry, node-count guards, key prefixes, and object-kind lookups
 * through the *active* profile instead of naming any specific model.
 *
 * Stage 0 scope: this interface only collects what was previously leaked
 * into core as qwen3 symbols (geometry, namespace, object-kind map,
 * placement struct). It does NOT parameterize OBMM layout — core keeps
 * using model-neutral layout aliases defined in mem_service_object_contract.h.
 *
 * Adding a new model: implement a static const profile and register it in
 * the profile table (mem_service_profile.c). Do not specialize the core.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations; full defs live in mem_service.h / mem_service_record_table.h. */
struct mem_service;
struct mem_service_record;

/*
 * Layer-range placement descriptor for one node within a pipeline.
 *
 * This is a typedef alias of the existing qwen3 placement struct so the
 * adapter and the core share one layout without the core naming qwen3.
 * The qwen3 adapter header includes this file and keeps its own struct
 * name as a compatibility alias (see mem_service_qwen3_placement.h).
 */
struct mem_service_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

/*
 * Geometry + naming + object-kind map for one model family.
 *
 * Fields mirror the existing mem_service_qwen3_* geometry query surface
 * (mem_service_qwen3.h:17-33). The active profile is selected at startup;
 * for stage 0 only the qwen3 profile is registered.
 */
struct mem_service_model_profile {
    const char *name;            /* "qwen3" / "deepseek-v4-flash" */
    const char *key_namespace;   /* replaces hardcoded "qwen3/session/" */

    /*
     * Geometry queries. Each is a thin function pointer; the adapter fills
     * these by delegating to its own model-specific helpers. Signatures
     * match the existing mem_service_qwen3_* functions so stage 0 stays a
     * behavior-neutral rename/reroute.
     */
    uint32_t (*layer_count)(void);
    uint32_t (*range_nodes)(void);
    uint64_t (*hidden_range_bytes)(void);
    uint64_t (*handoff_hidden_bytes)(uint64_t decode_step);
    uint64_t (*range_kv_state_bytes)(uint32_t layer_start,
                                     uint32_t layer_end,
                                     uint64_t token_count);
    const char *(*model_key)(void);
    int (*layer_range_for_node)(uint32_t local_node,
                                uint32_t cluster_node_count,
                                uint32_t *layer_start_out,
                                uint32_t *layer_end_out,
                                uint32_t *next_node_out);

    /*
     * OBMM object-kind map. Replaces direct MEM_SERVICE_OBMM_KIND_QWEN3_*
     * references in core. Values are the same integer kinds; core looks
     * them up via the profile.
     */
    uint32_t obmm_kind_token_result;
    uint64_t obmm_token_result_bytes;
    uint32_t obmm_kind_kv_state;
    uint32_t obmm_kind_engram_history;
    uint32_t obmm_kind_engram_candidates;
    uint32_t obmm_kind_engram_selected;
    uint32_t obmm_kind_engram_state;

    /*
     * Record recycling fallback used when the record table is full.
     * Adapters own their own eviction policy; core calls this through the
     * profile instead of naming a specific model's recycler.
     */
    struct mem_service_record *(*recycle_runtime_record)(struct mem_service *svc,
                                                         const char *incoming_key);

    /*
     * Layer-range placement service. Adapters persist/read placement
     * records (owner_node → layer range) through these callbacks. The
     * placement struct layout is shared (struct mem_service_layer_range_placement
     * above); adapters may alias it to their own compat name.
     */
    int (*publish_layer_range_placements)(struct mem_service *svc,
                                          uint32_t node_count);
    bool (*read_layer_range_placement)(struct mem_service *svc,
                                       uint32_t owner_node,
                                       struct mem_service_layer_range_placement *out);
    bool (*find_layer_range_predecessor)(struct mem_service *svc,
                                         uint32_t owner_node,
                                         struct mem_service_layer_range_placement *out);
};

/*
 * Active profile. Selected by the launch entry point from the existing
 * SIM_UAPI_W5_PROFILE / SIM_UAPI_W4_CHIPBACKEND_PROFILE workload names;
 * stage 0 introduces no new user-visible env. Defaults to "qwen3" so the
 * unset case matches today's behavior exactly.
 */
void mem_service_set_active_model_profile_name(const char *name);

const struct mem_service_model_profile *
mem_service_active_model_profile(void);

/*
 * Compile-time registry. Returns NULL for unknown names.
 * For stage 0 only "qwen3" is registered.
 */
const struct mem_service_model_profile *
mem_service_lookup_model_profile(const char *name);

/*
 * Convenience accessors over the active profile. These replace direct
 * core calls to mem_service_qwen3_* geometry/kind helpers.
 */
uint32_t mem_service_model_layer_count(void);
uint32_t mem_service_model_range_nodes(void);
uint64_t mem_service_model_hidden_range_bytes(void);
uint64_t mem_service_model_handoff_hidden_bytes(uint64_t decode_step);
uint64_t mem_service_model_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count);
const char *mem_service_model_key(void);
int mem_service_model_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out);

/*
 * Record-recycling fallback through the active profile. Returns NULL if no
 * adapter recycler is configured.
 */
struct mem_service_record *
mem_service_model_recycle_runtime_record(struct mem_service *svc,
                                         const char *incoming_key);

/* Placement service accessors over the active profile. */
int mem_service_model_publish_layer_range_placements(struct mem_service *svc,
                                                     uint32_t node_count);
bool mem_service_model_read_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out);
bool mem_service_model_find_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out);

/*
 * Human-readable name for an OBMM object kind. Returns "unknown" for kinds
 * not owned by the active profile; the neutral core kinds (weight_tile,
 * kvcache_block, hidden_range_*) are always recognized.
 */
const char *mem_service_object_kind_name(uint32_t payload_kind);

#endif
