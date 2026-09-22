/* SPDX-License-Identifier: MIT */
#ifndef DEEPSEEK_V4_FLASH_MANAGED_DIRECT_GUEST_H
#define DEEPSEEK_V4_FLASH_MANAGED_DIRECT_GUEST_H

#include "lingqu_model_direct_guest.h"

#define DEEPSEEK_V4_FLASH_MANAGED_DIRECT_CALLABLE_ID UINT64_C(5)
#define DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT 6u
#define DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT 23u

enum deepseek_v4_flash_managed_direct_tensor {
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_HIDDEN_INPUT = 0,
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_PREVIOUS_KV = 1,
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TOKEN_IDS = 2,
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_HIDDEN_OUTPUT = 3,
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_NEXT_KV = 4,
    DEEPSEEK_V4_FLASH_MANAGED_DIRECT_LOGITS = 5,
};

struct deepseek_v4_flash_managed_direct_geometry {
    uint32_t first;
    uint32_t end;
    uint32_t layers;
    uint32_t past;
    uint32_t tokens;
    uint32_t hidden;
    uint32_t hc_mult;
    uint32_t vocab;
    uint32_t attention_heads;
    uint32_t kv_heads;
    uint32_t head_dim;
    uint32_t rope_head_dim;
    uint32_t q_lora_rank;
    uint32_t o_lora_rank;
    uint32_t output_groups;
    uint32_t experts;
    uint32_t experts_used;
    uint32_t shared_experts;
    uint32_t moe_intermediate;
    uint32_t sliding_window;
    uint32_t indexer_heads;
    uint32_t indexer_head_dim;
    uint32_t indexer_top_k;
};

struct deepseek_v4_flash_managed_direct_description {
    struct lingqu_shmem_memref_spec
        tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT];
    uint32_t scalars[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT];
};

struct deepseek_v4_flash_managed_direct_request {
    struct deepseek_v4_flash_managed_direct_geometry geometry;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    struct mem_service_mapping_owner
        *owners[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT];
    uint64_t offsets[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT];
    uint64_t op_id;
    uint64_t request_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    void *metadata;
    size_t metadata_bytes;
    uint64_t metadata_iova;
};

struct deepseek_v4_flash_managed_direct_operation;

/* Describes callable 5 exactly. Hidden tensors contain the four hyper-
 * connection streams as contiguous F32 rows. KV tensors contain the existing
 * versioned DeepSeek range-state byte encoding at the exact past/next length.
 * No tensor payload pointer crosses this interface. */
int deepseek_v4_flash_managed_direct_describe(
    const struct deepseek_v4_flash_managed_direct_geometry *geometry,
    struct deepseek_v4_flash_managed_direct_description *description_out);

int deepseek_v4_flash_managed_direct_prepare(
    const struct deepseek_v4_flash_managed_direct_request *request,
    struct deepseek_v4_flash_managed_direct_operation **operation_out);

int deepseek_v4_flash_managed_direct_submit(
    struct deepseek_v4_flash_managed_direct_operation *operation);

int deepseek_v4_flash_managed_direct_poll(
    struct deepseek_v4_flash_managed_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

int deepseek_v4_flash_managed_direct_release(
    struct deepseek_v4_flash_managed_direct_operation **operation);

#endif
