/* SPDX-License-Identifier: MIT */
#include "w5_managed_serving_guest.h"

#include "components/mem_service/mem_service_object_contract.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct w5_managed_serving_operation {
    struct w5_managed_serving_request request;
    struct w5_managed_model_operation *model;
    struct mem_service_mapping_lease *cpu;
    struct mem_service_mapping_access_view cpu_view;
    struct w5_managed_serving_result result;
    unsigned staged;
    bool submitted;
    bool completed;
    bool model_released;
    bool output_visible;
    bool sealed;
};

static int output_key(const struct w5_managed_serving_operation *operation,
    unsigned index, char *out, size_t capacity)
{
    static const char *const suffixes[W5_MANAGED_SERVING_MAX_OUTPUTS] = {
        "/result", "/hidden", "/kv", "/terminal",
    };
    const struct mem_service_client_allocation *allocation =
        w5_managed_object_allocation(operation->request.writer);
    int length;
    if (!allocation || index >= W5_MANAGED_SERVING_MAX_OUTPUTS) return -EINVAL;
    length = snprintf(out, capacity, "%s%s", allocation->key, suffixes[index]);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

int w5_managed_serving_prepare(
    const struct w5_managed_serving_request *request,
    struct w5_managed_serving_operation **operation_out)
{
    struct w5_managed_serving_operation *operation;
    struct mem_service_mapping_owner *owners[4];
    struct lingqu_object_ref_wire_v2 references[W5_MANAGED_SERVING_MAX_INPUTS];
    uint64_t offsets[4] = {0};
    uint64_t lengths[4] = {0};
    const struct mem_service_client_allocation *allocation;
    uint64_t control_checksum;
    int rc;
    if (!request || !operation_out || !request->writer || !request->compute ||
        !request->endpoint || request->input_count > W5_MANAGED_SERVING_MAX_INPUTS ||
        !request->op_id || !request->request_id || !request->artifact_fingerprint ||
        !request->requester_cna || !request->metadata || !request->metadata_bytes ||
        !request->metadata_iova || !request->output_capacity ||
        request->output_offset < W5_MANAGED_MODEL_CONTROL_BYTES ||
        request->prompt_prefix_bytes > W5_MANAGED_MODEL_PREFIX_BYTES ||
        (request->prompt_prefix_bytes && !request->prompt_prefix)) return -EINVAL;
    *operation_out = NULL;
    allocation = w5_managed_object_allocation(request->writer);
    if (!allocation || request->output_offset > allocation->size_bytes ||
        request->output_capacity > allocation->size_bytes - request->output_offset)
        return -ERANGE;
    operation = calloc(1, sizeof(*operation));
    if (!operation) return -ENOMEM;
    operation->request = *request;
    *operation_out = operation;
    owners[0] = w5_managed_object_owner(request->writer);
    owners[request->input_count + 1] = owners[0];
    offsets[request->input_count + 1] = request->output_offset;
    lengths[0] = W5_MANAGED_MODEL_CONTROL_BYTES;
    lengths[request->input_count + 1] = request->output_capacity;
    for (unsigned i = 0; i < request->input_count; ++i) {
        const struct lingqu_object_ref_wire_v2 *reference =
            w5_managed_object_reference(request->inputs[i]);
        owners[i + 1] = w5_managed_object_owner(request->inputs[i]);
        if (!reference || !owners[i + 1] ||
            lingqu_object_ref_v2_validate(reference) ||
            reference->access != LINGQU_OBJECT_REF_V2_READ) return -ESTALE;
        references[i] = *reference;
        lengths[i + 1] = reference->object.payload_bytes;
    }
    if (!owners[0]) return -ESTALE;
    rc = w5_managed_object_acquire_cpu(request->writer, 0,
        allocation->size_bytes,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &operation->cpu, &operation->cpu_view);
    if (rc) return rc;
    rc = w5_managed_model_control_encode(operation->cpu_view.base,
        W5_MANAGED_MODEL_CONTROL_BYTES, request->op_id, request->request_id,
        request->levels, request->prompt_prefix, request->prompt_prefix_bytes,
        references, request->input_count);
    if (rc) return rc;
    control_checksum = mem_service_provider_checksum64(operation->cpu_view.base,
        W5_MANAGED_MODEL_CONTROL_BYTES);
    rc = w5_managed_object_publish_range(request->writer, operation->cpu, 0,
        W5_MANAGED_MODEL_CONTROL_BYTES, control_checksum);
    if (rc) return rc;
    return w5_managed_model_prepare_views(request->compute, owners, offsets,
        lengths, request->input_count, request->op_id, request->request_id,
        request->artifact_fingerprint, request->requester_cna,
        request->metadata, request->metadata_bytes, request->metadata_iova,
        &operation->model);
}

int w5_managed_serving_submit(
    struct w5_managed_serving_operation *operation)
{
    int rc;
    if (!operation || !operation->model || operation->submitted) return -EINVAL;
    rc = w5_managed_model_submit(operation->model, operation->request.endpoint);
    if (!rc) operation->submitted = true;
    return rc;
}

int w5_managed_serving_poll(
    struct w5_managed_serving_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    int rc;
    if (!operation || !completion_out || !operation->submitted ||
        operation->completed) return -EINVAL;
    rc = w5_managed_model_poll(operation->model, &operation->result.completion);
    if (rc) return rc;
    operation->completed = true;
    *completion_out = operation->result.completion;
    return 0;
}

static int stage_next(struct w5_managed_serving_operation *operation,
    enum mem_service_wire_status *status_out)
{
    const struct llm_infer_range_result *decoded = &operation->result.decoded;
    struct mem_service_client_reference_view view = {0};
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    int rc = output_key(operation, operation->staged, key, sizeof(key));
    if (rc) return rc;
    switch (operation->staged) {
    case 0:
        view.offset = operation->request.output_offset;
        view.len = decoded->serialized_bytes;
        view.object_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT;
        break;
    case 1:
        view.offset = decoded->hidden.offset;
        view.len = decoded->hidden.bytes;
        view.object_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT;
        break;
    case 2:
        view.offset = decoded->kv.offset;
        view.len = decoded->kv.bytes;
        view.object_kind = MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE;
        break;
    case 3:
        view.offset = decoded->terminal_view.offset;
        view.len = decoded->terminal_view.bytes;
        view.object_kind = MEM_SERVICE_OBMM_KIND_MODEL_TERMINAL_RESULT;
        break;
    default:
        return -EINVAL;
    }
    view.owner_entity = (uint32_t)operation->request.expected.node;
    view.producer_entity = view.owner_entity;
    rc = w5_managed_writer_stage(operation->request.writer, operation->cpu,
        key, &view, &operation->result.references[operation->staged], status_out);
    if (!rc) ++operation->staged;
    return rc;
}

int w5_managed_serving_finish(
    struct w5_managed_serving_operation *operation,
    struct w5_managed_serving_result *result_out,
    enum mem_service_wire_status *status_out)
{
    unsigned required;
    int rc;
    if (!operation || !result_out || !status_out || !operation->completed ||
        operation->sealed ||
        !lingqu_shmem_pto_completion_succeeded(&operation->result.completion))
        return -EINVAL;
    if (!operation->model_released) {
        rc = w5_managed_model_release(operation->model);
        if (rc) return rc;
        operation->model = NULL;
        operation->model_released = true;
    }
    if (!operation->output_visible) {
        const unsigned char *result = operation->cpu_view.base;
        result += operation->request.output_offset;
        rc = llm_infer_decode_range_result(result,
            (size_t)operation->request.output_capacity,
            operation->request.output_offset, &operation->request.expected,
            &operation->result.decoded);
        if (rc) return rc;
        rc = w5_managed_object_wait_range(operation->request.writer,
            operation->cpu, operation->request.output_offset,
            operation->result.decoded.serialized_bytes,
            operation->result.decoded.serialized_checksum);
        if (rc) return rc;
        operation->output_visible = true;
    }
    required = operation->result.decoded.terminal_present ? 4U : 3U;
    while (operation->staged < required) {
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

int w5_managed_serving_release(
    struct w5_managed_serving_operation **operation_ptr)
{
    struct w5_managed_serving_operation *operation;
    int rc;
    if (!operation_ptr || !*operation_ptr) return -EINVAL;
    operation = *operation_ptr;
    if (operation->model) {
        rc = w5_managed_model_release(operation->model);
        if (rc) return rc;
        operation->model = NULL;
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
