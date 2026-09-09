/* SPDX-License-Identifier: MIT */
#ifndef QWEN3_PTO_GUEST_H
#define QWEN3_PTO_GUEST_H
#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto_endpoint.h"

struct qwen3_pto_guest_geometry {
    uint32_t first, end, layers, past, tokens;
    uint32_t hidden, intermediate, query_heads, kv_heads, head_dim, vocab;
    uint32_t scale_bits;
};

struct qwen3_pto_guest_operation;

/* W5 token-result checksum at byte offset zero for one range invocation.
 * piece contains checksum, raw byte count, word0 and word1 from the table. */
uint64_t qwen3_pto_sample_text_checksum(uint64_t step, uint64_t token,
                                      const uint64_t piece[4]);

struct qwen3_pto_guest_output {
    struct lingqu_shmem_mem_service_local_buffer hidden, kv, logits;
};

int qwen3_pto_guest_prepare(
    const struct qwen3_pto_guest_geometry *geometry,
    const struct mem_service_object_payload_view *hidden,
    const struct mem_service_object_payload_view *previous_kv,
    const uint32_t *token_ids, uint64_t request_id, uint64_t fingerprint,
    uint32_t requester_cna, struct qwen3_pto_guest_operation **operation);

int qwen3_pto_guest_submit(struct qwen3_pto_guest_operation *operation,
    uint64_t timeout_ms, struct qwen3_pto_guest_output *output,
    struct lingqu_shmem_pto_completion *completion);

/* Returns -EBUSY if submit returned without a terminal completion. */
int qwen3_pto_guest_release(struct qwen3_pto_guest_operation *operation);
#endif
