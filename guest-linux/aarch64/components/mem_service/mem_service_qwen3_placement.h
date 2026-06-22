#ifndef MEM_SERVICE_QWEN3_PLACEMENT_H
#define MEM_SERVICE_QWEN3_PLACEMENT_H

#include <stdbool.h>
#include <stdint.h>

struct mem_service_qwen3_layer_range_placement {
    uint32_t owner_node;
    uint32_t layer_start;
    uint32_t layer_end;
    uint32_t next_owner_node;
    uint32_t layer_count;
    bool terminal;
};

#endif
