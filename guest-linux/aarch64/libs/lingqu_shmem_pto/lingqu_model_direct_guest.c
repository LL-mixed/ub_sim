/* SPDX-License-Identifier: MIT */
#include "lingqu_model_direct_guest.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct lingqu_model_direct_operation {
    struct lingqu_shmem_mem_service_lease
        *leases[LINGQU_MODEL_DIRECT_MAX_MEMREFS];
    struct lingqu_shmem_memref *memrefs[LINGQU_MODEL_DIRECT_MAX_MEMREFS];
    struct lingqu_shmem_pto_memref_arg
        args[LINGQU_MODEL_DIRECT_MAX_MEMREFS];
    struct lingqu_shmem_pto_inflight *inflight;
    struct lingqu_shmem_pto_endpoint *endpoint;
    LingquPtoDispatchSlotV2 slot;
    struct lingqu_shmem_pto_completion completion;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t callable_id;
    uint32_t count;
    uint32_t begun;
    uint32_t completed;
    bool prepared;
    bool submitted;
    bool pending;
    bool received;
};

static int validate_request(const struct lingqu_model_direct_request *request)
{
    bool seen[LINGQU_MODEL_DIRECT_MAX_MEMREFS +
              LINGQU_MODEL_DIRECT_MAX_SCALARS] = {false};
    uint32_t total;

    if (!request || !request->compute || !request->endpoint ||
        !request->op_id || !request->request_id || !request->callable_id ||
        !request->artifact_fingerprint || !request->requester_cna ||
        !request->memrefs || !request->memref_count ||
        request->memref_count > LINGQU_MODEL_DIRECT_MAX_MEMREFS ||
        request->scalar_count > LINGQU_MODEL_DIRECT_MAX_SCALARS ||
        (request->scalar_count && !request->scalars) || !request->metadata ||
        !request->metadata_bytes || !request->metadata_iova)
        return -EINVAL;
    total = request->memref_count + request->scalar_count;
    for (uint32_t i = 0; i < request->memref_count; ++i) {
        const struct lingqu_model_direct_memref *arg = &request->memrefs[i];
        if (!arg->owner || arg->arg_index >= total || seen[arg->arg_index] ||
            !arg->spec.byte_length || !arg->spec.rank ||
            arg->spec.rank > LINGQU_SHMEM_MAX_RANK || !arg->spec.access)
            return -EINVAL;
        seen[arg->arg_index] = true;
    }
    for (uint32_t i = 0; i < request->scalar_count; ++i) {
        uint32_t index = request->scalars[i].arg_index;
        if (index >= total || seen[index]) return -EINVAL;
        seen[index] = true;
    }
    for (uint32_t i = 0; i < total; ++i)
        if (!seen[i]) return -EINVAL;
    return 0;
}

int lingqu_model_direct_prepare(
    const struct lingqu_model_direct_request *request,
    struct lingqu_model_direct_operation **operation_out)
{
    struct lingqu_model_direct_operation *operation;
    struct lingqu_shmem_pto_request dispatch;
    struct lingqu_shmem_pto_wire_result wire;
    int rc;

    if (!operation_out) return -EINVAL;
    *operation_out = NULL;
    rc = validate_request(request);
    if (rc) return rc;
    operation = calloc(1, sizeof(*operation));
    if (!operation) return -ENOMEM;
    operation->endpoint = request->endpoint;
    operation->op_id = request->op_id;
    operation->request_id = request->request_id;
    operation->callable_id = request->callable_id;
    operation->count = request->memref_count;
    *operation_out = operation;

    for (uint32_t i = 0; i < operation->count; ++i) {
        rc = lingqu_shmem_mem_service_acquire_owned(request->compute,
            request->memrefs[i].owner, &request->memrefs[i].spec,
            &operation->memrefs[i], &operation->leases[i]);
        if (rc) return rc;
        operation->args[i] = (struct lingqu_shmem_pto_memref_arg) {
            .memref = operation->memrefs[i],
            .arg_index = request->memrefs[i].arg_index,
        };
    }
    dispatch = (struct lingqu_shmem_pto_request) {
        .op_id = request->op_id,
        .request_id = request->request_id,
        .callable_id = request->callable_id,
        .artifact_fingerprint = request->artifact_fingerprint,
        .requester_cna = request->requester_cna,
        .memrefs = operation->args,
        .memref_count = operation->count,
        .scalars = request->scalars,
        .scalar_count = request->scalar_count,
    };
    rc = lingqu_shmem_pto_dispatch_prepare(&dispatch, request->metadata,
        request->metadata_bytes, request->metadata_iova, &operation->slot,
        &wire, &operation->inflight);
    if (!rc) operation->prepared = true;
    return rc;
}

int lingqu_model_direct_submit(
    struct lingqu_model_direct_operation *operation)
{
    int rc;
    if (!operation || !operation->prepared || operation->submitted ||
        !operation->endpoint)
        return -EINVAL;
    while (operation->begun < operation->count) {
        rc = lingqu_shmem_mem_service_lease_begin(
            operation->leases[operation->begun], operation->op_id);
        if (rc) return rc;
        ++operation->begun;
    }
    rc = lingqu_shmem_pto_endpoint_enqueue(operation->endpoint,
                                            &operation->slot);
    if (!rc) operation->submitted = operation->pending = true;
    return rc;
}

int lingqu_model_direct_poll(
    struct lingqu_model_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    int rc;
    if (!operation || !completion_out || !operation->pending ||
        operation->begun != operation->count)
        return -EINVAL;
    if (!operation->received) {
        rc = lingqu_shmem_pto_endpoint_poll(operation->endpoint,
            operation->op_id, &operation->completion);
        if (rc) return rc;
        if (operation->completion.op_id != operation->op_id) return -EPROTO;
        operation->received = true;
    }
    while (operation->completed < operation->count) {
        rc = lingqu_shmem_mem_service_lease_complete(
            operation->leases[operation->completed], operation->op_id);
        if (rc) return rc;
        ++operation->completed;
    }
    *completion_out = operation->completion;
    operation->pending = false;
    fprintf(stderr,
        "model-direct: op_id=%llu request_id=%llu callable=%llu tensors=%u "
        "model_tensor_address_space=UB_GM pto_instruction_io=1 "
        "shared_payload_staging_bytes=0 completion_status=%u\n",
        (unsigned long long)operation->op_id,
        (unsigned long long)operation->request_id,
        (unsigned long long)operation->callable_id, operation->count,
        operation->completion.status);
    return 0;
}

int lingqu_model_direct_release(
    struct lingqu_model_direct_operation **operation_ptr)
{
    struct lingqu_model_direct_operation *operation;
    int result = 0;
    if (!operation_ptr || !*operation_ptr) return -EINVAL;
    operation = *operation_ptr;
    if (operation->pending ||
        (operation->submitted && operation->begun != operation->completed))
        return -EBUSY;
    while (!operation->submitted && operation->completed < operation->begun) {
        int rc = lingqu_shmem_mem_service_lease_complete(
            operation->leases[operation->completed], operation->op_id);
        if (rc) return rc;
        ++operation->completed;
    }
    operation->prepared = false;
    if (operation->inflight) {
        lingqu_shmem_pto_dispatch_finish(operation->inflight);
        operation->inflight = NULL;
    }
    for (uint32_t i = 0; i < operation->count; ++i) {
        if (!operation->leases[i]) continue;
        int rc = lingqu_shmem_mem_service_release(operation->leases[i]);
        if (!rc) operation->leases[i] = NULL;
        else if (!result) result = rc;
    }
    if (result) return result;
    free(operation);
    *operation_ptr = NULL;
    return 0;
}
