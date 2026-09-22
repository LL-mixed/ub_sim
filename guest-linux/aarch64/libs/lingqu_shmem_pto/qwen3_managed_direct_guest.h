/* SPDX-License-Identifier: MIT */
#ifndef QWEN3_MANAGED_DIRECT_GUEST_H
#define QWEN3_MANAGED_DIRECT_GUEST_H

#include "lingqu_model_direct_guest.h"
#include "qwen3_pto_guest.h"

#define QWEN3_MANAGED_DIRECT_CALLABLE_ID UINT64_C(3)
#define QWEN3_MANAGED_DIRECT_TENSOR_COUNT 6u
#define QWEN3_MANAGED_DIRECT_SCALAR_COUNT 12u

enum qwen3_managed_direct_tensor {
    QWEN3_MANAGED_DIRECT_HIDDEN_INPUT = 0,
    QWEN3_MANAGED_DIRECT_PREVIOUS_KV = 1,
    QWEN3_MANAGED_DIRECT_TOKEN_IDS = 2,
    QWEN3_MANAGED_DIRECT_HIDDEN_OUTPUT = 3,
    QWEN3_MANAGED_DIRECT_NEXT_KV = 4,
    QWEN3_MANAGED_DIRECT_LOGITS = 5,
};

struct qwen3_managed_direct_description {
    struct lingqu_shmem_memref_spec tensors[QWEN3_MANAGED_DIRECT_TENSOR_COUNT];
    uint32_t scalars[QWEN3_MANAGED_DIRECT_SCALAR_COUNT];
};

struct qwen3_managed_direct_request {
    struct qwen3_pto_guest_geometry geometry;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    struct mem_service_mapping_owner
        *owners[QWEN3_MANAGED_DIRECT_TENSOR_COUNT];
    uint64_t offsets[QWEN3_MANAGED_DIRECT_TENSOR_COUNT];
    uint64_t op_id;
    uint64_t request_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    void *metadata;
    size_t metadata_bytes;
    uint64_t metadata_iova;
};

struct qwen3_managed_direct_operation;

/* Returns the exact callable-3 tensor and scalar ABI for one model range.
 * Serving integration uses the same description to size managed views before
 * allocation, so dispatch cannot silently disagree with the published views. */
int qwen3_managed_direct_describe(
    const struct qwen3_pto_guest_geometry *geometry,
    struct qwen3_managed_direct_description *description_out);

/* Tensor order is hidden input, previous KV, token IDs, hidden output,
 * next KV and logits. All six views refer to caller-owned Memory Service
 * mappings; token IDs and KV headers must be initialized before prepare.
 * A failed prepare may return a cleanup-only operation. */
int qwen3_managed_direct_prepare(
    const struct qwen3_managed_direct_request *request,
    struct qwen3_managed_direct_operation **operation_out);

int qwen3_managed_direct_submit(
    struct qwen3_managed_direct_operation *operation);

int qwen3_managed_direct_poll(
    struct qwen3_managed_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

int qwen3_managed_direct_release(
    struct qwen3_managed_direct_operation **operation);

#endif
