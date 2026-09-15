/* SPDX-License-Identifier: MIT */
#include "w5_managed_model_guest.h"
#include "lingqu_shmem_pto.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static void put_le(unsigned char *out, uint64_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) out[i] = (unsigned char)(value >> (8 * i));
}

int w5_managed_model_control_encode(void *buffer, size_t length,
    uint64_t op_id, uint64_t request_id, const uint32_t levels[8],
    const void *prefix, size_t prefix_length,
    const struct lingqu_object_ref_wire_v2 *references, unsigned input_count)
{
    unsigned char encoded[W5_MANAGED_MODEL_CONTROL_BYTES] = {0};
    if (!buffer || length != sizeof(encoded) || !op_id || !request_id || !levels ||
        prefix_length > W5_MANAGED_MODEL_PREFIX_BYTES || (prefix_length && !prefix) ||
        input_count > 2 || (input_count && !references)) return -EINVAL;
    put_le(encoded, W5_MANAGED_MODEL_CONTROL_MAGIC, 8);
    put_le(encoded + 8, 1, 4);
    put_le(encoded + 12, input_count, 4);
    put_le(encoded + 16, op_id, 8);
    put_le(encoded + 24, request_id, 8);
    for (unsigned i = 0; i < 8; ++i) put_le(encoded + 32 + i * 4, levels[i], 4);
    if (prefix_length) memcpy(encoded + 64, prefix, prefix_length);
    for (unsigned i = 0; i < input_count; ++i) {
        int rc = lingqu_object_ref_v2_encode(&references[i],
            encoded + 64 + W5_MANAGED_MODEL_PREFIX_BYTES + i * 256, 256);
        if (rc) return -EINVAL;
        if (references[i].access != 1 ||
            references[i].object.payload_bytes > W5_MANAGED_MODEL_MAX_VIEW_BYTES)
            return -EINVAL;
    }
    memcpy(buffer, encoded, sizeof(encoded));
    return 0;
}

struct w5_managed_model_operation {
    struct lingqu_shmem_mem_service_lease *leases[4];
    struct lingqu_shmem_pto_inflight *inflight;
    struct lingqu_shmem_pto_endpoint *endpoint;
    LingquPtoDispatchSlotV2 slot;
    uint64_t op_id;
    unsigned count, begun, completed;
    bool prepared, submitted, pending, received;
    struct lingqu_shmem_pto_completion completion;
};

int w5_managed_model_release(struct w5_managed_model_operation *op)
{
    int result = 0;
    if (!op) return 0;
    if (op->pending || op->begun != op->completed) return -EBUSY;
    op->prepared = false;
    if (op->inflight) {
        lingqu_shmem_pto_dispatch_finish(op->inflight);
        op->inflight = NULL;
    }
    for (unsigned i = 0; i < op->count; ++i) {
        if (!op->leases[i]) continue;
        int rc = lingqu_shmem_mem_service_release(op->leases[i]);
        if (!rc) op->leases[i] = NULL;
        else if (!result) result = rc;
    }
    if (!result) free(op);
    return result;
}

int w5_managed_model_prepare(struct lingqu_shmem_mem_service_context *context,
    struct mem_service_mapping_owner *const *owners, const uint64_t *lengths,
    unsigned input_count, uint64_t op_id, uint64_t request_id,
    uint64_t fingerprint, uint32_t requester_cna,
    void *metadata, size_t metadata_bytes, uint64_t metadata_iova,
    struct w5_managed_model_operation **operation_out)
{
    const uint64_t offsets[4] = {0};
    return w5_managed_model_prepare_views(context, owners, offsets, lengths,
        input_count, op_id, request_id, fingerprint, requester_cna,
        metadata, metadata_bytes, metadata_iova, operation_out);
}

int w5_managed_model_prepare_views(struct lingqu_shmem_mem_service_context *context,
    struct mem_service_mapping_owner *const *owners, const uint64_t *offsets,
    const uint64_t *lengths, unsigned input_count, uint64_t op_id, uint64_t request_id,
    uint64_t fingerprint, uint32_t requester_cna,
    void *metadata, size_t metadata_bytes, uint64_t metadata_iova,
    struct w5_managed_model_operation **operation_out)
{
    struct lingqu_shmem_pto_memref_arg args[4];
    struct lingqu_shmem_pto_wire_result wire;
    int rc;
    if (!operation_out) return -EINVAL;
    *operation_out = NULL;
    if (!context || !owners || !offsets || !lengths || input_count > 2 || !op_id ||
        !request_id || !fingerprint || !metadata || !metadata_iova ||
        lengths[0] != W5_MANAGED_MODEL_CONTROL_BYTES) return -EINVAL;
    for (unsigned i = 0; i < input_count + 2; ++i)
        if (!owners[i] || !lengths[i] || lengths[i] > W5_MANAGED_MODEL_MAX_VIEW_BYTES ||
            offsets[i] > UINT64_MAX - lengths[i])
            return -EINVAL;
    struct w5_managed_model_operation *op = calloc(1, sizeof(*op));
    if (!op) return -ENOMEM;
    op->count = input_count + 2;
    op->op_id = op_id;
    *operation_out = op;
    for (unsigned i = 0; i < op->count; ++i) {
        struct lingqu_shmem_memref_spec spec = {
            .byte_offset = offsets[i], .byte_length = lengths[i], .rank = 1, .dtype = 4,
            .shape = {(uint32_t)lengths[i]}, .strides = {1},
            .access = i + 1 == op->count ? LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ,
        };
        args[i].arg_index = i;
        rc = lingqu_shmem_mem_service_acquire_owned(context, owners[i], &spec,
                                                   &args[i].memref, &op->leases[i]);
        if (rc) return rc;
    }
    struct lingqu_shmem_pto_request request = {
        .op_id = op_id, .request_id = request_id,
        .callable_id = W5_MANAGED_MODEL_CALLABLE_ID,
        .artifact_fingerprint = fingerprint, .requester_cna = requester_cna,
        .memrefs = args, .memref_count = op->count,
    };
    rc = lingqu_shmem_pto_dispatch_prepare(&request, metadata, metadata_bytes,
        metadata_iova, &op->slot, &wire, &op->inflight);
    if (!rc) op->prepared = true;
    return rc;
}

int w5_managed_model_submit(struct w5_managed_model_operation *op,
    struct lingqu_shmem_pto_endpoint *endpoint)
{
    if (!op || !endpoint || !op->prepared || op->submitted ||
        (op->endpoint && op->endpoint != endpoint)) return -EINVAL;
    op->endpoint = endpoint;
    while (op->begun < op->count) {
        int rc = lingqu_shmem_mem_service_lease_begin(op->leases[op->begun], op->op_id);
        if (rc) return rc; /* Retry the same prepare without beginning pins twice. */
        ++op->begun;
    }
    int rc = lingqu_shmem_pto_endpoint_enqueue(endpoint, &op->slot);
    /* Endpoint enqueue rejects atomically, consuming no ID or slot on error.
     * Retain the begun leases and retry this operation if queue capacity frees. */
    if (!rc) op->submitted = op->pending = true;
    return rc;
}

int w5_managed_model_poll(struct w5_managed_model_operation *op,
    struct lingqu_shmem_pto_completion *completion_out)
{
    if (!op || !completion_out || !op->pending || op->begun != op->count) return -EINVAL;
    if (!op->received) {
        int rc = lingqu_shmem_pto_endpoint_poll(op->endpoint, op->op_id, &op->completion);
        if (rc) return rc;
        if (op->completion.op_id != op->op_id) return -EPROTO;
        op->received = true;
    }
    // Preserve an already consumed completion if a lease completion needs retry.
    while (op->completed < op->count) {
        int rc = lingqu_shmem_mem_service_lease_complete(op->leases[op->completed], op->op_id);
        if (rc) return rc;
        ++op->completed;
    }
    *completion_out = op->completion;
    op->pending = false;
    return 0;
}
