/* SPDX-License-Identifier: MIT */
#ifndef DEEPSEEK_V4_FLASH_MANAGED_SERVING_GUEST_H
#define DEEPSEEK_V4_FLASH_MANAGED_SERVING_GUEST_H

#include "deepseek_v4_flash_managed_direct_guest.h"
#include "w5_managed_object_guest.h"

#define DEEPSEEK_V4_FLASH_MANAGED_SERVING_OUTPUT_COUNT 4u

enum deepseek_v4_flash_managed_writer_view_index {
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_UNUSED_HIDDEN = 0,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_UNUSED_PREVIOUS_STATE = 1,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_TOKEN_IDS = 2,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_HIDDEN_OUTPUT = 3,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_NEXT_STATE = 4,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_LOGITS = 5,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_TOKEN_RESULT = 6,
    DEEPSEEK_V4_FLASH_MANAGED_WRITER_VIEW_COUNT = 7,
};

struct deepseek_v4_flash_managed_writer_view {
    uint64_t offset;
    uint64_t bytes;
};

struct deepseek_v4_flash_managed_writer_layout {
    struct deepseek_v4_flash_managed_writer_view
        views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_VIEW_COUNT];
    uint64_t bytes;
};

struct deepseek_v4_flash_managed_serving_request {
    struct w5_managed_object_guest *writer;
    struct w5_managed_object_guest *hidden_input;
    struct w5_managed_object_guest *previous_state;
    struct deepseek_v4_flash_managed_direct_geometry geometry;
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

struct deepseek_v4_flash_managed_serving_output {
    struct deepseek_v4_flash_managed_writer_layout layout;
    unsigned char *hidden;
    unsigned char *state;
    unsigned char *logits;
    uint64_t hidden_checksum;
    uint64_t state_checksum;
    uint64_t logits_checksum;
    uint64_t input_checksum;
};

struct deepseek_v4_flash_managed_serving_result {
    struct lingqu_object_ref_wire_v2
        references[DEEPSEEK_V4_FLASH_MANAGED_SERVING_OUTPUT_COUNT];
    unsigned reference_count;
};

struct deepseek_v4_flash_managed_serving_operation;

int deepseek_v4_flash_managed_serving_layout(
    const struct deepseek_v4_flash_managed_direct_geometry *geometry,
    uint64_t writer_base_offset,
    struct deepseek_v4_flash_managed_writer_layout *layout_out);

int deepseek_v4_flash_managed_serving_prepare(
    const struct deepseek_v4_flash_managed_serving_request *request,
    struct deepseek_v4_flash_managed_serving_operation **operation_out);

int deepseek_v4_flash_managed_serving_submit(
    struct deepseek_v4_flash_managed_serving_operation *operation);

int deepseek_v4_flash_managed_serving_poll(
    struct deepseek_v4_flash_managed_serving_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

int deepseek_v4_flash_managed_serving_complete(
    struct deepseek_v4_flash_managed_serving_operation *operation,
    struct deepseek_v4_flash_managed_serving_output *output_out);

int deepseek_v4_flash_managed_serving_publish(
    struct deepseek_v4_flash_managed_serving_operation *operation,
    const uint64_t token_words[8],
    struct deepseek_v4_flash_managed_serving_result *result_out,
    enum mem_service_wire_status *status_out);

int deepseek_v4_flash_managed_serving_release(
    struct deepseek_v4_flash_managed_serving_operation **operation);

#endif
