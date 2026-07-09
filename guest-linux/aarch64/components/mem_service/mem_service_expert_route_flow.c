#include "mem_service_expert_route_flow.h"

#include <stdio.h>
#include <string.h>

/*
 * Expert route flow (stage 2): weight-tile addressing and route-decision
 * record key construction. These are layer-internal helpers; the cross-layer
 * handoff interface is unchanged.
 *
 * The real expert weight fetch goes through mem_service's object store
 * (WEIGHT_TILE kind) resolved by these keys; the node-side cache
 * (mem_service_expert_cache) sits in front of those fetches. See plan
 * section 3.3 (weight provider / cache as node-side optimization layer).
 */

int mem_service_expert_weight_tile_key(char *out,
                                       size_t out_len,
                                       const char *model_key,
                                       uint32_t layer_id,
                                       uint32_t expert_id,
                                       const char *quant)
{
    int written;

    if (!out || out_len == 0 || !model_key || !quant) {
        return -1;
    }
    written = snprintf(out,
                       out_len,
                       "weights/%s/layer%u/expert%u/%s",
                       model_key,
                       layer_id,
                       expert_id,
                       quant);
    return (written < 0 || (size_t)written >= out_len) ? -1 : 0;
}

int mem_service_expert_route_record_key(char *out,
                                        size_t out_len,
                                        const char *model_key,
                                        uint32_t layer_id,
                                        uint32_t token_index)
{
    int written;

    if (!out || out_len == 0 || !model_key) {
        return -1;
    }
    written = snprintf(out,
                       out_len,
                       "route/%s/layer%u/token%u",
                       model_key,
                       layer_id,
                       token_index);
    return (written < 0 || (size_t)written >= out_len) ? -1 : 0;
}
