#include "mem_service_profile.h"

#include <stddef.h>
#include <string.h>

#include "mem_service.h"
#include "mem_service_deepseek_v4_flash.h"
#include "mem_service_qwen3.h"
#include "mem_service_record_table.h"

/*
 * Adapters expose their profile through a typed accessor. qwen3 (stage 0)
 * and deepseek-v4-flash (stage 1) are registered here. Lookup is a linear
 * scan over a small static table.
 */
const struct mem_service_model_profile *mem_service_qwen3_profile(void);

static const struct mem_service_model_profile *const *profile_table(size_t *count_out)
{
    static const struct mem_service_model_profile *table[2];
    static bool built;
    if (!built) {
        table[0] = mem_service_qwen3_profile();
        table[1] = mem_service_deepseek_v4_flash_profile();
        built = true;
    }
    *count_out = (table[0] ? 1U : 0U) + (table[1] ? 1U : 0U);
    return table;
}

static const struct mem_service_model_profile *g_active_profile;

const struct mem_service_model_profile *
mem_service_lookup_model_profile(const char *name)
{
    size_t count;
    size_t i;
    const struct mem_service_model_profile *const *table = profile_table(&count);

    if (!name) {
        return NULL;
    }
    for (i = 0; i < count; ++i) {
        if (table[i] && strcmp(table[i]->name, name) == 0) {
            return table[i];
        }
    }
    return NULL;
}

void mem_service_set_active_model_profile_name(const char *name)
{
    const struct mem_service_model_profile *found;
    size_t count;
    const struct mem_service_model_profile *const *table = profile_table(&count);

    if (!name || name[0] == '\0' || !count) {
        g_active_profile = count ? table[0] : NULL;
        return;
    }
    found = mem_service_lookup_model_profile(name);
    g_active_profile = found ? found : (count ? table[0] : NULL);
}

const struct mem_service_model_profile *
mem_service_active_model_profile(void)
{
    size_t count;
    const struct mem_service_model_profile *const *table = profile_table(&count);

    if (!g_active_profile) {
        g_active_profile = count ? table[0] : NULL;
    }
    return g_active_profile;
}

uint32_t mem_service_model_layer_count(void)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->layer_count ? p->layer_count() : 0U;
}

uint32_t mem_service_model_range_nodes(void)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->range_nodes ? p->range_nodes() : 0U;
}

uint64_t mem_service_model_hidden_range_bytes(void)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->hidden_range_bytes ? p->hidden_range_bytes() : 0ULL;
}

uint64_t mem_service_model_handoff_hidden_bytes(uint64_t decode_step)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->handoff_hidden_bytes ? p->handoff_hidden_bytes(decode_step) : 0ULL;
}

uint64_t mem_service_model_range_kv_state_bytes(uint32_t layer_start,
                                                uint32_t layer_end,
                                                uint64_t token_count)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->range_kv_state_bytes
        ? p->range_kv_state_bytes(layer_start, layer_end, token_count)
        : 0ULL;
}

const char *mem_service_model_key(void)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->model_key ? p->model_key() : "unknown";
}

int mem_service_model_layer_range_for_node(uint32_t local_node,
                                           uint32_t cluster_node_count,
                                           uint32_t *layer_start_out,
                                           uint32_t *layer_end_out,
                                           uint32_t *next_node_out)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->layer_range_for_node
        ? p->layer_range_for_node(local_node,
                                  cluster_node_count,
                                  layer_start_out,
                                  layer_end_out,
                                  next_node_out)
        : -1;
}

struct mem_service_record *
mem_service_model_recycle_runtime_record(struct mem_service *svc,
                                         const char *incoming_key)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->recycle_runtime_record
        ? p->recycle_runtime_record(svc, incoming_key)
        : NULL;
}

int mem_service_model_publish_layer_range_placements(struct mem_service *svc,
                                                     uint32_t node_count)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->publish_layer_range_placements
        ? p->publish_layer_range_placements(svc, node_count)
        : -1;
}

bool mem_service_model_read_layer_range_placement(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->read_layer_range_placement
        ? p->read_layer_range_placement(svc, owner_node, out)
        : false;
}

bool mem_service_model_find_layer_range_predecessor(
    struct mem_service *svc,
    uint32_t owner_node,
    struct mem_service_layer_range_placement *out)
{
    const struct mem_service_model_profile *p = mem_service_active_model_profile();

    return p && p->find_layer_range_predecessor
        ? p->find_layer_range_predecessor(svc, owner_node, out)
        : false;
}
