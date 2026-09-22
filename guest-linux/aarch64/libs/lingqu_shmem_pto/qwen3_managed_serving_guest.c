/* SPDX-License-Identifier: MIT */
#include "qwen3_managed_serving_guest.h"

#include "components/mem_service/mem_service_object_contract.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QWEN3_MANAGED_VIEW_ALIGNMENT UINT64_C(64)

struct qwen3_managed_serving_operation {
    struct qwen3_managed_serving_request request;
    struct qwen3_managed_direct_description description;
    struct qwen3_managed_writer_layout layout;
    struct qwen3_managed_direct_operation *direct;
    struct mem_service_mapping_lease *cpu;
    struct mem_service_mapping_access_view cpu_view;
    struct qwen3_managed_serving_output output;
    struct qwen3_managed_serving_result result;
    unsigned staged;
    bool submitted;
    bool completed;
    bool direct_released;
    bool output_visible;
    bool sealed;
};

static int align_up(uint64_t value, uint64_t alignment, uint64_t *out)
{
    uint64_t mask;
    if (!out || !alignment || (alignment & (alignment - 1U))) return -EINVAL;
    mask = alignment - 1U;
    if (value > UINT64_MAX - mask) return -EOVERFLOW;
    *out = (value + mask) & ~mask;
    return 0;
}

static int append_view(struct qwen3_managed_writer_layout *layout,
    enum qwen3_managed_writer_view_index index, uint64_t bytes,
    uint64_t *cursor)
{
    int rc;
    if (!layout || index >= QWEN3_MANAGED_WRITER_VIEW_COUNT || !bytes ||
        !cursor) return -EINVAL;
    rc = align_up(*cursor, QWEN3_MANAGED_VIEW_ALIGNMENT, cursor);
    if (rc) return rc;
    if (*cursor > UINT64_MAX - bytes) return -EOVERFLOW;
    layout->views[index] = (struct qwen3_managed_writer_view) {
        .offset = *cursor,
        .bytes = bytes,
    };
    *cursor += bytes;
    return 0;
}

int qwen3_managed_serving_layout(
    const struct qwen3_pto_guest_geometry *geometry,
    uint64_t writer_base_offset,
    struct qwen3_managed_writer_layout *layout_out)
{
    struct qwen3_managed_direct_description description;
    uint64_t cursor = writer_base_offset;
    int rc;

    if (!layout_out) return -EINVAL;
    memset(layout_out, 0, sizeof(*layout_out));
    rc = qwen3_managed_direct_describe(geometry, &description);
    if (rc) return rc;
    rc = append_view(layout_out, QWEN3_MANAGED_WRITER_UNUSED_HIDDEN,
        sizeof(uint32_t), &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_UNUSED_PREVIOUS_KV,
            sizeof(uint32_t), &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_TOKEN_IDS,
            description.tensors[QWEN3_MANAGED_DIRECT_TOKEN_IDS].byte_length,
            &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT,
            description.tensors[QWEN3_MANAGED_DIRECT_HIDDEN_OUTPUT].byte_length,
            &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_NEXT_KV,
            description.tensors[QWEN3_MANAGED_DIRECT_NEXT_KV].byte_length,
            &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_LOGITS,
            description.tensors[QWEN3_MANAGED_DIRECT_LOGITS].byte_length,
            &cursor);
    if (!rc)
        rc = append_view(layout_out, QWEN3_MANAGED_WRITER_TOKEN_RESULT,
            MEM_SERVICE_OBMM_MODEL_TOKEN_RESULT_BYTES, &cursor);
    if (!rc) layout_out->bytes = cursor;
    return rc;
}

static unsigned char *view_base(
    const struct qwen3_managed_serving_operation *operation,
    enum qwen3_managed_writer_view_index index)
{
    const struct qwen3_managed_writer_view *view =
        &operation->layout.views[index];
    return operation->cpu_view.base + view->offset - operation->cpu_view.offset;
}

static int validate_reader(struct w5_managed_object_guest *object,
    uint32_t kind, uint64_t bytes)
{
    const struct lingqu_object_ref_wire_v2 *reference;
    if (!object || !w5_managed_object_owner(object)) return -EINVAL;
    reference = w5_managed_object_reference(object);
    if (!reference || lingqu_object_ref_v2_validate(reference) ||
        reference->access != LINGQU_OBJECT_REF_V2_READ ||
        reference->object.object_kind != kind ||
        reference->object.payload_bytes != bytes) return -ESTALE;
    return 0;
}

static int initialize_writer(struct qwen3_managed_serving_operation *operation)
{
    const struct qwen3_pto_guest_geometry *geometry =
        &operation->request.geometry;
    const struct qwen3_managed_writer_view *tokens =
        &operation->layout.views[QWEN3_MANAGED_WRITER_TOKEN_IDS];
    const struct qwen3_managed_writer_view *next_kv =
        &operation->layout.views[QWEN3_MANAGED_WRITER_NEXT_KV];
    const struct mem_service_client_allocation *allocation =
        w5_managed_object_allocation(operation->request.writer);
    uint64_t kv_width = (uint64_t)geometry->kv_heads * geometry->head_dim;
    uint64_t total = (uint64_t)geometry->past + geometry->tokens;
    uint64_t stride = (10U + 2U * total * kv_width) * 4U;
    uint64_t checksum;
    int rc;

    if (!allocation || operation->layout.bytes > allocation->size_bytes)
        return -ERANGE;
    rc = w5_managed_object_acquire_cpu(operation->request.writer,
        operation->request.writer_base_offset,
        operation->layout.bytes - operation->request.writer_base_offset,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &operation->cpu, &operation->cpu_view);
    if (rc) return rc;
    memset(operation->cpu_view.base, 0, operation->cpu_view.len);
    if (!geometry->first) {
        for (uint32_t i = 0; i < geometry->tokens; ++i) {
            float token = (float)operation->request.token_ids[i];
            memcpy(view_base(operation, QWEN3_MANAGED_WRITER_TOKEN_IDS) +
                       i * sizeof(token),
                   &token, sizeof(token));
        }
    }
    for (uint32_t layer = geometry->first; layer < geometry->end; ++layer) {
        uint64_t header[] = {layer, total, total, total, kv_width};
        memcpy(view_base(operation, QWEN3_MANAGED_WRITER_NEXT_KV) +
                   (layer - geometry->first) * stride,
               header, sizeof(header));
    }
    checksum = mem_service_provider_checksum64(
        view_base(operation, QWEN3_MANAGED_WRITER_TOKEN_IDS), tokens->bytes);
    rc = w5_managed_object_publish_range(operation->request.writer,
        operation->cpu, tokens->offset, tokens->bytes, checksum);
    if (!rc) {
        checksum = mem_service_provider_checksum64(
            view_base(operation, QWEN3_MANAGED_WRITER_NEXT_KV), next_kv->bytes);
        rc = w5_managed_object_publish_range(operation->request.writer,
            operation->cpu, next_kv->offset, next_kv->bytes, checksum);
    }
    if (!rc) rc = mem_service_mapping_lease_release(operation->cpu);
    if (!rc) operation->cpu = NULL;
    return rc;
}

int qwen3_managed_serving_prepare(
    const struct qwen3_managed_serving_request *request,
    struct qwen3_managed_serving_operation **operation_out)
{
    struct qwen3_managed_serving_operation *operation;
    struct qwen3_managed_direct_request direct = {0};
    struct mem_service_mapping_owner *writer_owner;
    int rc;

    if (!request || !operation_out || !request->writer || !request->compute ||
        !request->endpoint || !request->op_id || !request->request_id ||
        !request->artifact_fingerprint || !request->requester_cna ||
        !request->metadata || !request->metadata_bytes ||
        !request->metadata_iova) return -EINVAL;
    *operation_out = NULL;
    operation = calloc(1, sizeof(*operation));
    if (!operation) return -ENOMEM;
    operation->request = *request;
    *operation_out = operation;
    rc = qwen3_managed_direct_describe(&request->geometry,
                                       &operation->description);
    if (rc) return rc;
    if ((!request->geometry.first &&
         (!request->token_ids || request->token_count != request->geometry.tokens)) ||
        (request->geometry.first && request->hidden_input == NULL) ||
        (request->geometry.past && request->previous_kv == NULL)) return -EINVAL;
    if (!request->geometry.first)
        for (uint32_t i = 0; i < request->token_count; ++i)
            if (request->token_ids[i] >= request->geometry.vocab) return -ERANGE;
    if (request->geometry.first) {
        rc = validate_reader(request->hidden_input,
            MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
            operation->description
                .tensors[QWEN3_MANAGED_DIRECT_HIDDEN_INPUT].byte_length);
        if (rc) return rc;
    }
    if (request->geometry.past) {
        rc = validate_reader(request->previous_kv,
            MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE,
            operation->description
                .tensors[QWEN3_MANAGED_DIRECT_PREVIOUS_KV].byte_length);
        if (rc) return rc;
    }
    rc = qwen3_managed_serving_layout(&request->geometry,
        request->writer_base_offset, &operation->layout);
    if (rc) return rc;
    writer_owner = w5_managed_object_owner(request->writer);
    if (!writer_owner) return -ESTALE;
    rc = initialize_writer(operation);
    if (rc) return rc;

    direct.geometry = request->geometry;
    direct.compute = request->compute;
    direct.endpoint = request->endpoint;
    direct.op_id = request->op_id;
    direct.request_id = request->request_id;
    direct.artifact_fingerprint = request->artifact_fingerprint;
    direct.requester_cna = request->requester_cna;
    direct.metadata = request->metadata;
    direct.metadata_bytes = request->metadata_bytes;
    direct.metadata_iova = request->metadata_iova;
    direct.owners[QWEN3_MANAGED_DIRECT_HIDDEN_INPUT] =
        request->geometry.first ?
            w5_managed_object_owner(request->hidden_input) : writer_owner;
    direct.offsets[QWEN3_MANAGED_DIRECT_HIDDEN_INPUT] =
        request->geometry.first ? 0 :
            operation->layout.views[QWEN3_MANAGED_WRITER_UNUSED_HIDDEN].offset;
    direct.owners[QWEN3_MANAGED_DIRECT_PREVIOUS_KV] =
        request->geometry.past ?
            w5_managed_object_owner(request->previous_kv) : writer_owner;
    direct.offsets[QWEN3_MANAGED_DIRECT_PREVIOUS_KV] =
        request->geometry.past ? 0 :
            operation->layout
                .views[QWEN3_MANAGED_WRITER_UNUSED_PREVIOUS_KV].offset;
    for (uint32_t i = QWEN3_MANAGED_DIRECT_TOKEN_IDS;
         i < QWEN3_MANAGED_DIRECT_TENSOR_COUNT; ++i)
        direct.owners[i] = writer_owner;
    direct.offsets[QWEN3_MANAGED_DIRECT_TOKEN_IDS] =
        operation->layout.views[QWEN3_MANAGED_WRITER_TOKEN_IDS].offset;
    direct.offsets[QWEN3_MANAGED_DIRECT_HIDDEN_OUTPUT] =
        operation->layout.views[QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT].offset;
    direct.offsets[QWEN3_MANAGED_DIRECT_NEXT_KV] =
        operation->layout.views[QWEN3_MANAGED_WRITER_NEXT_KV].offset;
    direct.offsets[QWEN3_MANAGED_DIRECT_LOGITS] =
        operation->layout.views[QWEN3_MANAGED_WRITER_LOGITS].offset;
    return qwen3_managed_direct_prepare(&direct, &operation->direct);
}

int qwen3_managed_serving_submit(
    struct qwen3_managed_serving_operation *operation)
{
    int rc;
    if (!operation || !operation->direct || operation->submitted)
        return -EINVAL;
    rc = qwen3_managed_direct_submit(operation->direct);
    if (!rc) operation->submitted = true;
    return rc;
}

int qwen3_managed_serving_poll(
    struct qwen3_managed_serving_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    int rc;
    if (!operation || !completion_out || !operation->submitted ||
        operation->completed) return -EINVAL;
    rc = qwen3_managed_direct_poll(operation->direct, completion_out);
    if (!rc) operation->completed = true;
    return rc;
}

static int wait_output(struct qwen3_managed_serving_operation *operation,
    enum qwen3_managed_writer_view_index index, uint64_t *checksum_out)
{
    const struct qwen3_managed_writer_view *view = &operation->layout.views[index];
    uint64_t checksum = mem_service_provider_checksum64(
        view_base(operation, index), view->bytes);
    int rc = w5_managed_object_wait_range(operation->request.writer,
        operation->cpu, view->offset, view->bytes, checksum);
    if (!rc) *checksum_out = checksum;
    return rc;
}

int qwen3_managed_serving_complete(
    struct qwen3_managed_serving_operation *operation,
    struct qwen3_managed_serving_output *output_out)
{
    const struct lingqu_object_ref_wire_v2 *hidden_reference;
    const struct qwen3_managed_writer_view *first;
    const struct qwen3_managed_writer_view *last;
    int rc;

    if (!operation || !output_out || !operation->completed ||
        operation->output_visible || operation->sealed) return -EINVAL;
    if (!operation->direct_released) {
        rc = qwen3_managed_direct_release(&operation->direct);
        if (rc) return rc;
        operation->direct_released = true;
    }
    first = &operation->layout.views[QWEN3_MANAGED_WRITER_UNUSED_HIDDEN];
    last = &operation->layout.views[QWEN3_MANAGED_WRITER_TOKEN_RESULT];
    if (!operation->cpu) {
        rc = w5_managed_object_acquire_cpu(operation->request.writer,
            first->offset, last->offset + last->bytes - first->offset,
            MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
            &operation->cpu, &operation->cpu_view);
        if (rc) return rc;
    }
    operation->output.layout = operation->layout;
    operation->output.hidden =
        view_base(operation, QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT);
    operation->output.kv = view_base(operation, QWEN3_MANAGED_WRITER_NEXT_KV);
    operation->output.logits = view_base(operation, QWEN3_MANAGED_WRITER_LOGITS);
    rc = wait_output(operation, QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT,
                     &operation->output.hidden_checksum);
    if (!rc)
        rc = wait_output(operation, QWEN3_MANAGED_WRITER_NEXT_KV,
                         &operation->output.kv_checksum);
    if (!rc && operation->request.geometry.end ==
                   operation->request.geometry.layers)
        rc = wait_output(operation, QWEN3_MANAGED_WRITER_LOGITS,
                         &operation->output.logits_checksum);
    if (rc) return rc;
    if (operation->request.geometry.first) {
        hidden_reference =
            w5_managed_object_reference(operation->request.hidden_input);
        if (!hidden_reference) return -ESTALE;
        operation->output.input_checksum =
            hidden_reference->object.payload_checksum;
    } else {
        const struct qwen3_managed_writer_view *tokens =
            &operation->layout.views[QWEN3_MANAGED_WRITER_TOKEN_IDS];
        operation->output.input_checksum = mem_service_provider_checksum64(
            view_base(operation, QWEN3_MANAGED_WRITER_TOKEN_IDS),
            tokens->bytes);
    }
    operation->output_visible = true;
    *output_out = operation->output;
    return 0;
}

static int output_key(const struct qwen3_managed_serving_operation *operation,
    unsigned index, char *out, size_t capacity)
{
    static const char *const suffixes[QWEN3_MANAGED_SERVING_OUTPUT_COUNT] = {
        "/hidden", "/kv", "/logits", "/token",
    };
    const struct mem_service_client_allocation *allocation =
        w5_managed_object_allocation(operation->request.writer);
    int length;
    if (!allocation || index >= QWEN3_MANAGED_SERVING_OUTPUT_COUNT)
        return -EINVAL;
    length = snprintf(out, capacity, "%s%s", allocation->key, suffixes[index]);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

static int stage_next(struct qwen3_managed_serving_operation *operation,
    enum mem_service_wire_status *status_out)
{
    static const enum qwen3_managed_writer_view_index indices[] = {
        QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT,
        QWEN3_MANAGED_WRITER_NEXT_KV,
        QWEN3_MANAGED_WRITER_LOGITS,
        QWEN3_MANAGED_WRITER_TOKEN_RESULT,
    };
    static const uint32_t kinds[] = {
        MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
        MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE,
        MEM_SERVICE_OBMM_KIND_MODEL_TERMINAL_RESULT,
        MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT,
    };
    const struct qwen3_managed_writer_view *layout_view;
    struct mem_service_client_reference_view view = {0};
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    int rc;

    if (operation->staged >= QWEN3_MANAGED_SERVING_OUTPUT_COUNT)
        return -EINVAL;
    rc = output_key(operation, operation->staged, key, sizeof(key));
    if (rc) return rc;
    layout_view = &operation->layout.views[indices[operation->staged]];
    view.offset = layout_view->offset;
    view.len = layout_view->bytes;
    view.object_kind = kinds[operation->staged];
    view.owner_entity = operation->request.producer_node;
    view.producer_entity = view.owner_entity;
    rc = w5_managed_writer_stage(operation->request.writer, operation->cpu,
        key, &view, &operation->result.references[operation->staged],
        status_out);
    if (!rc) ++operation->staged;
    return rc;
}

int qwen3_managed_serving_publish(
    struct qwen3_managed_serving_operation *operation,
    const uint64_t token_words[8],
    struct qwen3_managed_serving_result *result_out,
    enum mem_service_wire_status *status_out)
{
    bool terminal;
    unsigned required;
    int rc;

    if (!operation || !result_out || !status_out ||
        !operation->output_visible || operation->sealed) return -EINVAL;
    terminal = operation->request.geometry.end ==
        operation->request.geometry.layers;
    required = terminal ? QWEN3_MANAGED_SERVING_OUTPUT_COUNT : 2U;
    if (terminal && operation->staged < required) {
        const struct qwen3_managed_writer_view *token =
            &operation->layout.views[QWEN3_MANAGED_WRITER_TOKEN_RESULT];
        uint64_t checksum;
        if (!operation->cpu || !token_words ||
            token_words[0] != operation->request.decode_step ||
            token_words[1] >= operation->request.geometry.vocab) return -EINVAL;
        memcpy(view_base(operation, QWEN3_MANAGED_WRITER_TOKEN_RESULT),
               token_words, token->bytes);
        checksum = mem_service_provider_checksum64(
            view_base(operation, QWEN3_MANAGED_WRITER_TOKEN_RESULT),
            token->bytes);
        rc = w5_managed_object_publish_range(operation->request.writer,
            operation->cpu, token->offset, token->bytes, checksum);
        if (rc) return rc;
    }
    while (operation->staged < required) {
        if (!operation->cpu) return -EINVAL;
        rc = stage_next(operation, status_out);
        if (rc) return rc;
    }
    operation->result.reference_count = required;
    if (operation->cpu) {
        rc = mem_service_mapping_lease_release(operation->cpu);
        if (rc) return rc;
        operation->cpu = NULL;
    }
    rc = w5_managed_writer_seal(operation->request.writer, status_out);
    if (rc) return rc;
    operation->sealed = true;
    *result_out = operation->result;
    return 0;
}

int qwen3_managed_serving_release(
    struct qwen3_managed_serving_operation **operation_ptr)
{
    struct qwen3_managed_serving_operation *operation;
    int rc;
    if (!operation_ptr || !*operation_ptr) return -EINVAL;
    operation = *operation_ptr;
    if (operation->direct) {
        rc = qwen3_managed_direct_release(&operation->direct);
        if (rc) return rc;
    }
    if (operation->cpu) {
        rc = mem_service_mapping_lease_release(operation->cpu);
        if (rc) return rc;
        operation->cpu = NULL;
    }
    free(operation);
    *operation_ptr = NULL;
    return 0;
}
