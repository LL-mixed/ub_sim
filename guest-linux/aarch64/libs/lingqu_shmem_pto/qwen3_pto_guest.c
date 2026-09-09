/* SPDX-License-Identifier: MIT */
#include "qwen3_pto_guest.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_sim.h"
#include "components/mem_service/mem_service.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define QWEN3_PTO_METADATA_BYTES 4096u

static uint64_t qwen3_pto_rotate_left(uint64_t value, unsigned int bits)
{
    return (value << bits) | (value >> (64u - bits));
}

uint64_t qwen3_pto_sample_text_checksum(uint64_t step, uint64_t token,
                                      const uint64_t piece[4])
{
    return (UINT64_C(0xcbf29ce484222325) * UINT64_C(0x100000001b3) +
            qwen3_pto_rotate_left(step, 11)) ^
           qwen3_pto_rotate_left(token, 31) ^ qwen3_pto_rotate_left(piece[1], 23) ^
           qwen3_pto_rotate_left(piece[2], 37) ^ qwen3_pto_rotate_left(piece[3], 43) ^
           qwen3_pto_rotate_left(piece[0], 3);
}

struct qwen3_pto_guest_operation {
    struct lingqu_shmem_mem_service_context *context;
    struct lingqu_shmem_mem_service_lease *leases[6];
    struct lingqu_shmem_memref *memrefs[6];
    struct lingqu_shmem_mem_service_local_buffer buffers[6];
    struct lingqu_shmem_pto_inflight *inflight;
    struct lingqu_shmem_pto_endpoint *endpoint;
    void *metadata;
    LingquPtoDispatchSlotV2 slot;
    int submitted, pending;
};

int qwen3_pto_guest_release(struct qwen3_pto_guest_operation *op)
{
    int result = 0;
    if (!op) return 0;
    if (op->pending) return -EBUSY;
    if (op->endpoint) lingqu_shmem_pto_endpoint_close(op->endpoint);
    if (op->inflight) lingqu_shmem_pto_dispatch_finish(op->inflight);
    for (unsigned int i = 0; i < 6; ++i) {
        if (op->leases[i]) {
            int rc = lingqu_shmem_mem_service_release(op->leases[i]);
            if (rc && !result) result = rc;
        }
    }
    if (op->context) {
        int rc = lingqu_shmem_mem_service_close(op->context);
        if (rc && !result) result = rc;
    }
    if (op->metadata) munmap(op->metadata, QWEN3_PTO_METADATA_BYTES);
    free(op);
    return result;
}

int qwen3_pto_guest_prepare(
    const struct qwen3_pto_guest_geometry *g,
    const struct mem_service_object_payload_view *hidden,
    const struct mem_service_object_payload_view *previous_kv,
    const uint32_t *token_ids, uint64_t request_id, uint64_t fingerprint,
    uint32_t requester_cna, struct qwen3_pto_guest_operation **operation)
{
    struct qwen3_pto_guest_operation *op;
    struct lingqu_shmem_pto_memref_arg args[6];
    struct lingqu_shmem_pto_scalar_desc scalars[12];
    struct lingqu_shmem_pto_request request;
    struct lingqu_shmem_pto_wire_result wire;
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    uint64_t previous_words, next_words, kv_width, metadata_iova;
    uint32_t shape[6][2], scalar_values[12];
    float scale;
    int rc;

    if (!operation) return -EINVAL;
    *operation = NULL;
    if (!g || !request_id || !fingerprint || !requester_cna || g->first >= g->end ||
        g->end > g->layers || g->layers > 128 || !g->tokens ||
        (uint64_t)g->past + g->tokens > 4096 || !g->hidden || g->hidden > 4096 ||
        !g->intermediate || g->intermediate > 4096 || !g->query_heads || !g->kv_heads ||
        g->query_heads % g->kv_heads || !g->head_dim || g->head_dim % 2 ||
        (uint64_t)g->query_heads * g->head_dim > 4096 || !g->vocab || g->vocab > (1u << 24))
        return -EINVAL;
    memcpy(&scale, &g->scale_bits, sizeof(scale));
    if (!isfinite(scale) || fabsf(scale - 1.0f / sqrtf((float)g->head_dim)) > 1e-7f)
        return -EINVAL;
    if ((g->first && !hidden) || (g->past && !previous_kv) || (!g->first && !token_ids))
        return -EINVAL;
    if (!g->first)
        for (uint32_t i = 0; i < g->tokens; ++i)
            if (token_ids[i] >= g->vocab) return -ERANGE;
    kv_width = (uint64_t)g->kv_heads * g->head_dim;
    previous_words = (g->end - g->first) * (10 + 2 * (uint64_t)g->past * kv_width);
    next_words = (g->end - g->first) * (10 + 2 * ((uint64_t)g->past + g->tokens) * kv_width);
    if (previous_words > UINT32_MAX || next_words > UINT32_MAX) return -EOVERFLOW;
    shape[0][0] = g->first ? g->tokens : 1;
    shape[0][1] = g->first ? g->hidden : 1;
    shape[1][0] = 1; shape[1][1] = g->past ? (uint32_t)previous_words : 1;
    shape[2][0] = 1; shape[2][1] = g->first ? 1 : g->tokens;
    shape[3][0] = g->tokens; shape[3][1] = g->hidden;
    shape[4][0] = 1; shape[4][1] = (uint32_t)next_words;
    shape[5][0] = 1; shape[5][1] = g->end == g->layers ? g->vocab : 1;

    if (g->past) {
        uint64_t stride = (10 + 2 * (uint64_t)g->past * kv_width) * 4;
        if (!previous_kv->data || previous_kv->len != previous_words * 4) return -EINVAL;
        for (uint32_t layer = g->first; layer < g->end; ++layer) {
            uint64_t header[5];
            memcpy(header, previous_kv->data + (layer - g->first) * stride, sizeof(header));
            if (header[0] != layer || header[1] != g->past || header[2] != g->past ||
                header[3] != g->past || header[4] != kv_width) return -EINVAL;
        }
    }
    op = calloc(1, sizeof(*op));
    if (!op) return -ENOMEM;
    rc = lingqu_shmem_mem_service_open(&op->context);
    if (rc) goto fail;
    for (uint32_t i = 0; i < 6; ++i) {
        uint16_t dtype = (i == 3 || (i == 0 && g->first)) ? 1 : 0;
        struct lingqu_shmem_memref_spec spec = {
            .byte_length = (uint64_t)shape[i][0] * shape[i][1] * (dtype ? 2 : 4),
            .rank = 2, .dtype = dtype,
            .access = i == 4 ? LINGQU_SHMEM_ACCESS_READ_WRITE :
                      (i == 3 || (i == 5 && g->end == g->layers) ?
                       LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ),
            .shape = {shape[i][0], shape[i][1]}, .strides = {shape[i][1], 1},
        };
        const struct mem_service_object_payload_view *input =
            i == 0 && g->first ? hidden : (i == 1 && g->past ? previous_kv : NULL);
        if (input) {
            if (!input->data || input->len != spec.byte_length) { rc = -EINVAL; goto fail; }
            rc = lingqu_shmem_mem_service_acquire(op->context, input, &spec, &op->memrefs[i], &op->leases[i]);
        } else if (i == 4) {
            rc = lingqu_shmem_mem_service_acquire_local_kv(op->context, &spec,
                &op->memrefs[i], &op->buffers[i], &op->leases[i]);
        } else {
            rc = lingqu_shmem_mem_service_acquire_local(op->context, &spec, &op->memrefs[i],
                                                       &op->buffers[i], &op->leases[i]);
        }
        if (rc) goto fail;
        args[i] = (struct lingqu_shmem_pto_memref_arg){.memref = op->memrefs[i], .arg_index = i};
        if (i == 2 && !g->first) {
            for (uint32_t t = 0; t < g->tokens; ++t) {
                float id = (float)token_ids[t];
                memcpy(op->buffers[i].data + t * sizeof(id), &id, sizeof(id));
            }
        } else if (i == 4) {
            uint64_t total = (uint64_t)g->past + g->tokens;
            uint64_t stride = (10 + 2 * total * kv_width) * 4;
            for (uint32_t layer = g->first; layer < g->end; ++layer) {
                uint64_t header[] = {layer, total, total, total, kv_width};
                memcpy(op->buffers[i].data + (layer - g->first) * stride, header, sizeof(header));
            }
        }
    }
    uint32_t values[] = {g->first, g->end, g->layers, g->past, g->tokens, g->hidden,
        g->intermediate, g->query_heads, g->kv_heads, g->head_dim, g->vocab, g->scale_bits};
    memcpy(scalar_values, values, sizeof(values));
    for (uint32_t i = 0; i < 12; ++i)
        scalars[i] = (struct lingqu_shmem_pto_scalar_desc){.arg_index = 6 + i, .dtype = 8, .value = scalar_values[i]};
    request = (struct lingqu_shmem_pto_request){.op_id = request_id, .request_id = request_id,
        .callable_id = 3, .artifact_fingerprint = fingerprint, .requester_cna = requester_cna,
        .memrefs = args, .memref_count = 6, .scalars = scalars, .scalar_count = 12};
    op->metadata = mmap(NULL, QWEN3_PTO_METADATA_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (op->metadata == MAP_FAILED) { op->metadata = NULL; rc = -errno; goto fail; }
    /* Fault in the private page before asking pagemap for its physical PFN. */
    memset(op->metadata, 0, QWEN3_PTO_METADATA_BYTES);
    rc = lingqu_shmem_sim_phys_for_virt(op->metadata, &metadata_iova);
    if (rc) goto fail;
    rc = lingqu_shmem_pto_dispatch_prepare(&request, op->metadata, QWEN3_PTO_METADATA_BYTES,
                                          metadata_iova, &op->slot, &wire, &op->inflight);
    if (rc) goto fail;
    rc = lingqu_shmem_pto_endpoint_open(&op->endpoint, &endpoint_info);
    if (rc) goto fail;
    *operation = op;
    return 0;
fail:
    qwen3_pto_guest_release(op);
    return rc;
}

int qwen3_pto_guest_submit(struct qwen3_pto_guest_operation *op,
    uint64_t timeout_ms, struct qwen3_pto_guest_output *output,
    struct lingqu_shmem_pto_completion *completion)
{
    int rc;
    if (!op || !output || !completion || !timeout_ms || op->submitted) return -EINVAL;
    memset(output, 0, sizeof(*output));
    memset(completion, 0, sizeof(*completion));
    op->submitted = 1;
    op->pending = 1;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rc = lingqu_shmem_pto_endpoint_submit(op->endpoint, &op->slot, timeout_ms, completion);
    if (rc) return rc;
    op->pending = 0;
    if (!lingqu_shmem_pto_completion_succeeded(completion)) return -EIO;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    output->hidden = op->buffers[3];
    output->kv = op->buffers[4];
    output->logits = op->buffers[5];
    return 0;
}
