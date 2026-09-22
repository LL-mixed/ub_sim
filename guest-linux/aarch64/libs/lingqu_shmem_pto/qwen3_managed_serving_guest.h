/* SPDX-License-Identifier: MIT */
#ifndef QWEN3_MANAGED_SERVING_GUEST_H
#define QWEN3_MANAGED_SERVING_GUEST_H

#include "qwen3_managed_direct_guest.h"
#include "w5_managed_object_guest.h"

#define QWEN3_MANAGED_SERVING_OUTPUT_COUNT 4u

enum qwen3_managed_writer_view_index {
    QWEN3_MANAGED_WRITER_UNUSED_HIDDEN = 0,
    QWEN3_MANAGED_WRITER_UNUSED_PREVIOUS_KV = 1,
    QWEN3_MANAGED_WRITER_TOKEN_IDS = 2,
    QWEN3_MANAGED_WRITER_HIDDEN_OUTPUT = 3,
    QWEN3_MANAGED_WRITER_NEXT_KV = 4,
    QWEN3_MANAGED_WRITER_LOGITS = 5,
    QWEN3_MANAGED_WRITER_TOKEN_RESULT = 6,
    QWEN3_MANAGED_WRITER_VIEW_COUNT = 7,
};

struct qwen3_managed_writer_view {
    uint64_t offset;
    uint64_t bytes;
};

struct qwen3_managed_writer_layout {
    struct qwen3_managed_writer_view views[QWEN3_MANAGED_WRITER_VIEW_COUNT];
    uint64_t bytes;
};

struct qwen3_managed_serving_request {
    struct w5_managed_object_guest *writer;
    struct w5_managed_object_guest *hidden_input;
    struct w5_managed_object_guest *previous_kv;
    struct qwen3_pto_guest_geometry geometry;
    const uint32_t *token_ids;
    uint32_t token_count;
    uint64_t writer_base_offset;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    uint32_t producer_node;
    uint64_t decode_step;
    void *metadata;
    size_t metadata_bytes;
    uint64_t metadata_iova;
};

struct qwen3_managed_serving_output {
    struct qwen3_managed_writer_layout layout;
    unsigned char *hidden;
    unsigned char *kv;
    unsigned char *logits;
    uint64_t hidden_checksum;
    uint64_t kv_checksum;
    uint64_t logits_checksum;
    uint64_t input_checksum;
};

struct qwen3_managed_serving_result {
    struct lingqu_object_ref_wire_v2
        references[QWEN3_MANAGED_SERVING_OUTPUT_COUNT];
    unsigned reference_count;
};

struct qwen3_managed_serving_operation;

/* The layout contains all writer-owned callable inputs and outputs. Every view
 * starts on a 64-byte boundary and the returned byte count is allocation
 * relative, including writer_base_offset. */
int qwen3_managed_serving_layout(
    const struct qwen3_pto_guest_geometry *geometry,
    uint64_t writer_base_offset,
    struct qwen3_managed_writer_layout *layout_out);

/* Initializes token IDs and next-KV headers in the managed writer, publishes
 * those initialized ranges, releases the CPU lease, then pins the actual
 * hidden/KV reader owners and writer owner for callable 3. */
int qwen3_managed_serving_prepare(
    const struct qwen3_managed_serving_request *request,
    struct qwen3_managed_serving_operation **operation_out);

int qwen3_managed_serving_submit(
    struct qwen3_managed_serving_operation *operation);

int qwen3_managed_serving_poll(
    struct qwen3_managed_serving_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

/* Releases callable-3 compute leases after its CQ completion, acquires one CPU
 * view of the same writer and establishes visibility for model outputs. The
 * returned pointers remain valid until publish or release. */
int qwen3_managed_serving_complete(
    struct qwen3_managed_serving_operation *operation,
    struct qwen3_managed_serving_output *output_out);

/* Stages hidden/KV and terminal logits/token views from the same allocation,
 * releases the CPU lease and seals the writer. token_words is required only
 * for a terminal range and uses the eight-word Memory Service token ABI. */
int qwen3_managed_serving_publish(
    struct qwen3_managed_serving_operation *operation,
    const uint64_t token_words[8],
    struct qwen3_managed_serving_result *result_out,
    enum mem_service_wire_status *status_out);

int qwen3_managed_serving_release(
    struct qwen3_managed_serving_operation **operation);

#endif
