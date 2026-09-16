/* SPDX-License-Identifier: MIT */
#ifndef W5_MANAGED_SERVING_GUEST_H
#define W5_MANAGED_SERVING_GUEST_H

#include "w5_managed_model_guest.h"
#include "w5_managed_object_guest.h"
#include "components/llm_infer/llm_infer.h"

#define W5_MANAGED_SERVING_MAX_INPUTS 2u
#define W5_MANAGED_SERVING_MAX_OUTPUTS 5u

struct w5_managed_serving_request {
    struct w5_managed_object_guest *writer;
    struct w5_managed_object_guest *inputs[W5_MANAGED_SERVING_MAX_INPUTS];
    unsigned input_count;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    uint32_t levels[8];
    const void *prompt_prefix;
    size_t prompt_prefix_bytes;
    uint64_t output_offset;
    uint64_t output_capacity;
    void *metadata;
    size_t metadata_bytes;
    uint64_t metadata_iova;
    struct llm_infer_range_identity expected;
};

struct w5_managed_serving_result {
    struct lingqu_shmem_pto_completion completion;
    struct llm_infer_range_result decoded;
    struct lingqu_object_ref_wire_v2
        references[W5_MANAGED_SERVING_MAX_OUTPUTS];
    unsigned reference_count;
};

struct w5_managed_serving_operation;

/* Prepare publishes immutable control, pins all reader/writer owners and builds
 * callable 4. Inputs remain caller-owned until operation release. */
int w5_managed_serving_prepare(
    const struct w5_managed_serving_request *request,
    struct w5_managed_serving_operation **operation_out);

int w5_managed_serving_submit(
    struct w5_managed_serving_operation *operation);

/* -EAGAIN means the original operation remains live and must be polled again. */
int w5_managed_serving_poll(
    struct w5_managed_serving_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

/* After successful completion: release compute pins, establish CPU visibility,
 * decode exact bounds, stage result/hidden/KV and optional terminal/token V2
 * views, release the CPU pin, then atomically seal the writer allocation.
 * Retry the same operation after any uncertain service result. */
int w5_managed_serving_finish(
    struct w5_managed_serving_operation *operation,
    struct w5_managed_serving_result *result_out,
    enum mem_service_wire_status *status_out);

/* Release only local operation state. It refuses an in-flight callable. Reader
 * objects and the writer are closed separately by their owners. */
int w5_managed_serving_release(
    struct w5_managed_serving_operation **operation);

#endif
