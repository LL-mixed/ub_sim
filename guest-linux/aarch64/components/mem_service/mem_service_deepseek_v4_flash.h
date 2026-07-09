#ifndef MEM_SERVICE_DEEPSEEK_V4_FLASH_H
#define MEM_SERVICE_DEEPSEEK_V4_FLASH_H

/*
 * DeepSeek V4 Flash model adapter for mem_service.
 *
 * Stage 1 scope: geometry only (layer count, hidden size, range nodes, MoE
 * expert counts). Real MoE routing / expert aggregation / expert cache is
 * stage 2. Values mirror DwarfStar (ds4) DS4_SHAPE_FLASH
 * (/Volumes/repos/ds4/ds4.c:177-212), the algorithmic reference.
 *
 * Flash is a Mixture-of-Experts transformer: 43 layers, hidden 4096, 256
 * routed experts (top-6 active) + 1 shared expert, compressed sparse
 * attention (128-token raw sliding window, ratio-4 / ratio-128 compressed
 * layers). For the pipeline-parallel sharding modeled here only the layer
 * count and range node count matter; per-layer KV coefficients differ by
 * layer type but stay constants (see the plan section 3.4).
 */

#include "mem_service_profile.h"

/*
 * Flash profile accessor. Bundles geometry queries, object-kind map, and
 * placement callbacks into a const struct registered in the profile table.
 * Stage 1 reuses the same OBMM object kinds as qwen3 (the layout is shared
 * until stage 1's layout-split predecessor); only the geometry differs.
 */
const struct mem_service_model_profile *mem_service_deepseek_v4_flash_profile(void);

#endif
