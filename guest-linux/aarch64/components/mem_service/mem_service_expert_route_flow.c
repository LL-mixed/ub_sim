#include "mem_service_expert_route_flow.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define MEM_SERVICE_EXPERT_FNV_OFFSET 0xcbf29ce484222325ULL
#define MEM_SERVICE_EXPERT_FNV_PRIME 0x100000001b3ULL

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

static uint64_t mem_service_expert_hash_bytes(uint64_t hash,
                                              const uint8_t *bytes,
                                              size_t len)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        hash ^= (uint64_t)bytes[i];
        hash *= MEM_SERVICE_EXPERT_FNV_PRIME;
    }
    return hash;
}

static uint64_t mem_service_expert_stable_hash_bytes(const uint8_t *bytes,
                                                     size_t len)
{
    return mem_service_expert_hash_bytes(MEM_SERVICE_EXPERT_FNV_OFFSET,
                                         bytes,
                                         len);
}

static uint64_t mem_service_expert_hash_u64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8];
    size_t i;

    for (i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = (uint8_t)((value >> (i * 8U)) & 0xffU);
    }
    return mem_service_expert_hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t mem_service_expert_stable_checksum_words(const uint64_t *words,
                                                         size_t count)
{
    uint64_t hash = MEM_SERVICE_EXPERT_FNV_OFFSET;
    size_t i;

    for (i = 0; i < count; ++i) {
        uint8_t bytes[8];
        size_t j;

        for (j = 0; j < sizeof(bytes); ++j) {
            bytes[j] = (uint8_t)((words[i] >> (j * 8U)) & 0xffU);
        }
        hash = mem_service_expert_hash_bytes(hash, bytes, sizeof(bytes));
    }
    return hash;
}

static uint64_t mem_service_expert_weight_tile_checksum(const char *model_key,
                                                        uint32_t layer_id,
                                                        uint32_t expert_id,
                                                        const char *quant,
                                                        uint64_t payload_bytes)
{
    uint64_t words[5];

    words[0] = mem_service_expert_stable_hash_bytes((const uint8_t *)model_key,
                                                    strlen(model_key));
    words[1] = (uint64_t)layer_id;
    words[2] = (uint64_t)expert_id;
    words[3] = mem_service_expert_stable_hash_bytes((const uint8_t *)quant,
                                                    strlen(quant));
    words[4] = payload_bytes;
    return mem_service_expert_stable_checksum_words(words,
                                                    sizeof(words) / sizeof(words[0]));
}

static int mem_service_expert_weight_tile_ref_init_with_checksum(
    struct mem_service_expert_weight_tile_ref *ref,
    const char *model_key,
    uint32_t layer_id,
    uint32_t expert_id,
    const char *quant,
    uint64_t payload_bytes,
    uint64_t payload_checksum)
{
    int written;

    if (!ref || !model_key || model_key[0] == '\0' || !quant ||
        quant[0] == '\0' || payload_bytes == 0 ||
        payload_checksum == 0 ||
        expert_id >= MEM_SERVICE_EXPERT_MAX_EXPERTS) {
        return -1;
    }
    memset(ref, 0, sizeof(*ref));
    written = snprintf(ref->model_key, sizeof(ref->model_key), "%s", model_key);
    if (written < 0 || (size_t)written >= sizeof(ref->model_key)) {
        return -1;
    }
    written = snprintf(ref->quant, sizeof(ref->quant), "%s", quant);
    if (written < 0 || (size_t)written >= sizeof(ref->quant)) {
        return -1;
    }
    if (mem_service_expert_weight_tile_key(ref->object_key,
                                           sizeof(ref->object_key),
                                           model_key,
                                           layer_id,
                                           expert_id,
                                           quant) != 0) {
        return -1;
    }
    ref->layer_id = layer_id;
    ref->expert_id = expert_id;
    ref->payload_bytes = payload_bytes;
    ref->payload_checksum = payload_checksum;
    return 0;
}

int mem_service_expert_weight_tile_ref_init(
    struct mem_service_expert_weight_tile_ref *ref,
    const char *model_key,
    uint32_t layer_id,
    uint32_t expert_id,
    const char *quant,
    uint64_t payload_bytes)
{
    uint64_t payload_checksum;

    if (!model_key || !quant || payload_bytes == 0) {
        return -1;
    }
    payload_checksum = mem_service_expert_weight_tile_checksum(model_key,
                                                              layer_id,
                                                              expert_id,
                                                              quant,
                                                              payload_bytes);
    return mem_service_expert_weight_tile_ref_init_with_checksum(ref,
                                                                 model_key,
                                                                 layer_id,
                                                                 expert_id,
                                                                 quant,
                                                                 payload_bytes,
                                                                 payload_checksum);
}

int mem_service_expert_weight_tile_ref_from_catalog_file(
    struct mem_service_expert_weight_tile_ref *ref,
    const char *catalog_path,
    const char *model_key,
    uint32_t layer_id,
    uint32_t expert_id)
{
    FILE *file;
    char line[512];
    char header_source_kind[64];
    char header_model_key[64];
    char header_checksum_algorithm[64];
    unsigned int header_layers = 0;
    unsigned int header_experts = 0;
    int header_seen = 0;

    if (!ref || !catalog_path || catalog_path[0] == '\0' ||
        !model_key || model_key[0] == '\0' ||
        expert_id >= MEM_SERVICE_EXPERT_MAX_EXPERTS) {
        return -1;
    }
    file = fopen(catalog_path, "r");
    if (!file) {
        return -1;
    }
    while (fgets(line, sizeof(line), file)) {
        unsigned int tile_layer = 0;
        unsigned int tile_expert = 0;
        unsigned long long payload_bytes = 0;
        unsigned long long payload_checksum = 0;
        char quant[16];

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        if (!header_seen && strncmp(line, "source_kind=", 12) == 0) {
            if (sscanf(line,
                       "source_kind=%63s model_key=%63s total_layers=%u experts_per_layer=%u checksum_algorithm=%63s",
                       header_source_kind,
                       header_model_key,
                       &header_layers,
                       &header_experts,
                       header_checksum_algorithm) != 5) {
                fclose(file);
                return -1;
            }
            if (strcmp(header_model_key, model_key) != 0 ||
                layer_id >= header_layers ||
                expert_id >= header_experts ||
                header_experts > MEM_SERVICE_EXPERT_MAX_EXPERTS ||
                strcmp(header_checksum_algorithm, "deterministic-v1") != 0) {
                fclose(file);
                return -1;
            }
            header_seen = 1;
            continue;
        }
        if (strncmp(line, "tile ", 5) != 0) {
            continue;
        }
        if (!header_seen) {
            fclose(file);
            return -1;
        }
        memset(quant, 0, sizeof(quant));
        if (sscanf(line,
                   "tile layer=%u expert=%u quant=%15s payload_bytes=%llu payload_checksum=0x%llx",
                   &tile_layer,
                   &tile_expert,
                   quant,
                   &payload_bytes,
                   &payload_checksum) != 5) {
            fclose(file);
            return -1;
        }
        if (tile_layer != layer_id || tile_expert != expert_id) {
            continue;
        }
        fclose(file);
        return mem_service_expert_weight_tile_ref_init_with_checksum(
            ref,
            model_key,
            layer_id,
            expert_id,
            quant,
            (uint64_t)payload_bytes,
            (uint64_t)payload_checksum);
    }
    fclose(file);
    return -1;
}

static void mem_service_expert_sort_u32(uint32_t *values, uint32_t count)
{
    uint32_t i;

    for (i = 1; i < count; ++i) {
        uint32_t value = values[i];
        uint32_t j = i;

        while (j > 0 && values[j - 1] > value) {
            values[j] = values[j - 1];
            --j;
        }
        values[j] = value;
    }
}

static int mem_service_expert_seen(const uint32_t *values,
                                   uint32_t count,
                                   uint32_t value)
{
    uint32_t i;

    for (i = 0; i < count; ++i) {
        if (values[i] == value) {
            return 1;
        }
    }
    return 0;
}

int mem_service_expert_route_decision_init(
    struct mem_service_expert_route_decision *decision,
    uint64_t step_index,
    uint32_t layer_id,
    uint32_t token_index,
    const uint32_t *active_experts,
    uint32_t active_expert_count)
{
    uint32_t i;

    if (!decision || !active_experts ||
        active_expert_count == 0 ||
        active_expert_count > MEM_SERVICE_EXPERT_ROUTE_TOP_K) {
        return -1;
    }
    memset(decision, 0, sizeof(*decision));
    decision->step_index = step_index;
    decision->layer_id = layer_id;
    decision->token_index = token_index;
    decision->active_expert_count = active_expert_count;
    for (i = 0; i < active_expert_count; ++i) {
        if (active_experts[i] >= MEM_SERVICE_EXPERT_MAX_EXPERTS ||
            mem_service_expert_seen(decision->active_experts,
                                    i,
                                    active_experts[i])) {
            memset(decision, 0, sizeof(*decision));
            return -1;
        }
        decision->active_experts[i] = active_experts[i];
    }
    mem_service_expert_sort_u32(decision->active_experts,
                                decision->active_expert_count);
    return 0;
}

int mem_service_expert_route_decision_for_decode(
    struct mem_service_expert_route_decision *decision,
    uint64_t step_index,
    uint32_t layer_id,
    uint32_t token_index,
    uint32_t expert_count)
{
    uint32_t selected[MEM_SERVICE_EXPERT_ROUTE_TOP_K];
    uint32_t selected_count = 0;
    uint32_t salt = 0;

    if (!decision ||
        expert_count < MEM_SERVICE_EXPERT_ROUTE_TOP_K ||
        expert_count > MEM_SERVICE_EXPERT_MAX_EXPERTS) {
        return -1;
    }
    while (selected_count < MEM_SERVICE_EXPERT_ROUTE_TOP_K) {
        uint64_t hash = MEM_SERVICE_EXPERT_FNV_OFFSET;
        uint32_t candidate;

        hash = mem_service_expert_hash_u64(hash, step_index);
        hash = mem_service_expert_hash_u64(hash, (uint64_t)layer_id);
        hash = mem_service_expert_hash_u64(hash, (uint64_t)token_index);
        hash = mem_service_expert_hash_u64(hash, (uint64_t)salt);
        candidate = (uint32_t)(hash % (uint64_t)expert_count);
        ++salt;
        if (mem_service_expert_seen(selected, selected_count, candidate)) {
            continue;
        }
        selected[selected_count++] = candidate;
    }
    return mem_service_expert_route_decision_init(decision,
                                                  step_index,
                                                  layer_id,
                                                  token_index,
                                                  selected,
                                                  selected_count);
}

int mem_service_expert_route_record_key(char *out,
                                        size_t out_len,
                                        const char *model_key,
                                        uint64_t step_index,
                                        uint32_t layer_id,
                                        uint32_t token_index)
{
    int written;

    if (!out || out_len == 0 || !model_key) {
        return -1;
    }
    written = snprintf(out,
                       out_len,
                       "route/%s/step%" PRIu64 "/layer%u/token%u",
                       model_key,
                       step_index,
                       layer_id,
                       token_index);
    return (written < 0 || (size_t)written >= out_len) ? -1 : 0;
}
