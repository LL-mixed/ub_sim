/* SPDX-License-Identifier: MIT */
#ifndef QWEN3_MANAGED_DIRECT_GUEST_H
#define QWEN3_MANAGED_DIRECT_GUEST_H

#include "lingqu_model_direct_guest.h"
#include "qwen3_pto_guest.h"

#define QWEN3_MANAGED_DIRECT_CALLABLE_ID UINT64_C(3)
#define QWEN3_MANAGED_DIRECT_TENSOR_COUNT 6u

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
