/* SPDX-License-Identifier: MIT */
#include "qwen3_managed_direct_guest.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct qwen3_managed_direct_operation {
    struct lingqu_model_direct_operation *direct;
};

static int qwen3_geometry(
    const struct qwen3_pto_guest_geometry *geometry,
    uint32_t shapes[QWEN3_MANAGED_DIRECT_TENSOR_COUNT][2],
    uint32_t scalar_values[12])
{
    uint64_t kv_width, previous_words, next_words;
    float scale;

    if (!geometry || geometry->first >= geometry->end ||
        geometry->end > geometry->layers || geometry->layers > 128 ||
        !geometry->tokens ||
        (uint64_t)geometry->past + geometry->tokens > 4096 ||
        !geometry->hidden || geometry->hidden > 4096 ||
        !geometry->intermediate || geometry->intermediate > 4096 ||
        !geometry->query_heads || !geometry->kv_heads ||
        geometry->query_heads % geometry->kv_heads ||
        !geometry->head_dim || geometry->head_dim % 2 ||
        (uint64_t)geometry->query_heads * geometry->head_dim > 4096 ||
        !geometry->vocab || geometry->vocab > (1u << 24))
        return -EINVAL;
    memcpy(&scale, &geometry->scale_bits, sizeof(scale));
    if (!isfinite(scale) ||
        fabsf(scale - 1.0f / sqrtf((float)geometry->head_dim)) > 1e-7f)
        return -EINVAL;
    kv_width = (uint64_t)geometry->kv_heads * geometry->head_dim;
    previous_words = (geometry->end - geometry->first) *
        (10 + 2 * (uint64_t)geometry->past * kv_width);
    next_words = (geometry->end - geometry->first) *
        (10 + 2 * ((uint64_t)geometry->past + geometry->tokens) * kv_width);
    if (previous_words > UINT32_MAX || next_words > UINT32_MAX)
        return -EOVERFLOW;
    shapes[0][0] = geometry->first ? geometry->tokens : 1;
    shapes[0][1] = geometry->first ? geometry->hidden : 1;
    shapes[1][0] = 1;
    shapes[1][1] = geometry->past ? (uint32_t)previous_words : 1;
    shapes[2][0] = 1;
    shapes[2][1] = geometry->first ? 1 : geometry->tokens;
    shapes[3][0] = geometry->tokens;
    shapes[3][1] = geometry->hidden;
    shapes[4][0] = 1;
    shapes[4][1] = (uint32_t)next_words;
    shapes[5][0] = 1;
    shapes[5][1] = geometry->end == geometry->layers ? geometry->vocab : 1;
    scalar_values[0] = geometry->first;
    scalar_values[1] = geometry->end;
    scalar_values[2] = geometry->layers;
    scalar_values[3] = geometry->past;
    scalar_values[4] = geometry->tokens;
    scalar_values[5] = geometry->hidden;
    scalar_values[6] = geometry->intermediate;
    scalar_values[7] = geometry->query_heads;
    scalar_values[8] = geometry->kv_heads;
    scalar_values[9] = geometry->head_dim;
    scalar_values[10] = geometry->vocab;
    scalar_values[11] = geometry->scale_bits;
    return 0;
}

int qwen3_managed_direct_prepare(
    const struct qwen3_managed_direct_request *request,
    struct qwen3_managed_direct_operation **operation_out)
{
    struct qwen3_managed_direct_operation *operation;
    struct lingqu_model_direct_memref
        memrefs[QWEN3_MANAGED_DIRECT_TENSOR_COUNT];
    struct lingqu_shmem_pto_scalar_desc scalars[12];
    struct lingqu_model_direct_request direct;
    uint32_t shapes[QWEN3_MANAGED_DIRECT_TENSOR_COUNT][2];
    uint32_t values[12];
    int rc;

    if (!operation_out) return -EINVAL;
    *operation_out = NULL;
    if (!request || !request->compute || !request->endpoint ||
        !request->op_id || !request->request_id ||
        !request->artifact_fingerprint || !request->requester_cna ||
        !request->metadata || !request->metadata_bytes ||
        !request->metadata_iova)
        return -EINVAL;
    rc = qwen3_geometry(&request->geometry, shapes, values);
    if (rc) return rc;
    for (uint32_t i = 0; i < QWEN3_MANAGED_DIRECT_TENSOR_COUNT; ++i) {
        uint16_t dtype = (i == 3 || (i == 0 && request->geometry.first)) ?
            1 : 0;
        uint64_t width = dtype ? 2 : 4;
        if (!request->owners[i] ||
            (uint64_t)shapes[i][0] > UINT64_MAX / shapes[i][1] ||
            (uint64_t)shapes[i][0] * shapes[i][1] > UINT64_MAX / width)
            return -EINVAL;
        memrefs[i] = (struct lingqu_model_direct_memref) {
            .owner = request->owners[i],
            .spec = {
                .byte_offset = request->offsets[i],
                .byte_length =
                    (uint64_t)shapes[i][0] * shapes[i][1] * width,
                .rank = 2,
                .dtype = dtype,
                .access = i == 4 ? LINGQU_SHMEM_ACCESS_READ_WRITE :
                    (i == 3 || (i == 5 &&
                     request->geometry.end == request->geometry.layers) ?
                     LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ),
                .shape = {shapes[i][0], shapes[i][1]},
                .strides = {shapes[i][1], 1},
            },
            .arg_index = i,
        };
    }
    for (uint32_t i = 0; i < 12; ++i) {
        scalars[i] = (struct lingqu_shmem_pto_scalar_desc) {
            .arg_index = QWEN3_MANAGED_DIRECT_TENSOR_COUNT + i,
            .dtype = 8,
            .value = values[i],
        };
    }
    operation = calloc(1, sizeof(*operation));
    if (!operation) return -ENOMEM;
    *operation_out = operation;
    direct = (struct lingqu_model_direct_request) {
        .compute = request->compute,
        .endpoint = request->endpoint,
        .op_id = request->op_id,
        .request_id = request->request_id,
        .callable_id = QWEN3_MANAGED_DIRECT_CALLABLE_ID,
        .artifact_fingerprint = request->artifact_fingerprint,
        .requester_cna = request->requester_cna,
        .memrefs = memrefs,
        .memref_count = QWEN3_MANAGED_DIRECT_TENSOR_COUNT,
        .scalars = scalars,
        .scalar_count = 12,
        .metadata = request->metadata,
        .metadata_bytes = request->metadata_bytes,
        .metadata_iova = request->metadata_iova,
    };
    return lingqu_model_direct_prepare(&direct, &operation->direct);
}

int qwen3_managed_direct_submit(
    struct qwen3_managed_direct_operation *operation)
{
    if (!operation || !operation->direct) return -EINVAL;
    return lingqu_model_direct_submit(operation->direct);
}

int qwen3_managed_direct_poll(
    struct qwen3_managed_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    if (!operation || !operation->direct) return -EINVAL;
    return lingqu_model_direct_poll(operation->direct, completion_out);
}

int qwen3_managed_direct_release(
    struct qwen3_managed_direct_operation **operation_ptr)
{
    struct qwen3_managed_direct_operation *operation;
    int rc;
    if (!operation_ptr || !*operation_ptr) return -EINVAL;
    operation = *operation_ptr;
    if (operation->direct) {
        rc = lingqu_model_direct_release(&operation->direct);
        if (rc) return rc;
    }
    free(operation);
    *operation_ptr = NULL;
    return 0;
}
