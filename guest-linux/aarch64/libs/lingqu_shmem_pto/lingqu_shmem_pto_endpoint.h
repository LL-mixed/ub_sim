/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_ENDPOINT_H
#define LINGQU_SHMEM_PTO_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

#include <linqu_shmem_pto_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINGQU_SHMEM_PTO_COMPLETION_BYTES 64u
#define LINGQU_SHMEM_PTO_COMPLETION_CODE_BYTES 45u

struct lingqu_shmem_pto_endpoint;

struct lingqu_shmem_pto_endpoint_info {
    char resource_path[128];
    uint64_t root_version;
    uint64_t default_segment;
    uint32_t cmdq_depth;
    uint32_t cq_depth;
};

struct lingqu_shmem_pto_completion {
    uint64_t op_id;
    uint64_t finished_at;
    uint8_t task_marker;
    uint8_t source;
    uint8_t status;
    char error_code[LINGQU_SHMEM_PTO_COMPLETION_CODE_BYTES];
};

int lingqu_shmem_pto_completion_decode(
    const uint8_t slot[LINGQU_SHMEM_PTO_COMPLETION_BYTES],
    struct lingqu_shmem_pto_completion *completion);

bool lingqu_shmem_pto_completion_succeeded(
    const struct lingqu_shmem_pto_completion *completion);

int lingqu_shmem_pto_endpoint_open(
    struct lingqu_shmem_pto_endpoint **endpoint_out,
    struct lingqu_shmem_pto_endpoint_info *info_out);

/* Nonblocking multi-request API. Serialize individual calls externally, but
 * do not hold that serialization while waiting between polls. Each successful
 * enqueue copies the dispatch slot only. The caller must retain its metadata,
 * memrefs and leases until poll returns that request's valid completion.
 * IDs must increase for this endpoint's lifetime; rejected enqueues consume no
 * ID. Capacity includes completed requests not yet collected by their caller.
 * Do not mix these calls with a pending synchronous submission.
 */
int lingqu_shmem_pto_endpoint_enqueue(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot);

/* Return -EAGAIN while pending; -ENOENT if the ID is not owned. A protocol
 * fault returns -EPROTO and retains all uncollected requests and queue storage.
 */
int lingqu_shmem_pto_endpoint_poll(
    struct lingqu_shmem_pto_endpoint *endpoint,
    uint64_t op_id,
    struct lingqu_shmem_pto_completion *completion);

uint32_t lingqu_shmem_pto_endpoint_queued_count(
    const struct lingqu_shmem_pto_endpoint *endpoint);

int lingqu_shmem_pto_endpoint_submit(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    struct lingqu_shmem_pto_completion *completion);

int lingqu_shmem_pto_endpoint_submit_cancel_after(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    uint64_t cancel_after_ms,
    struct lingqu_shmem_pto_completion *completion);

/* Wait for the original submission after a timeout; never enqueue it again.
 * Keep metadata and memrefs alive until this returns a valid completion.
 * All endpoint operations require external serialization.
 */
int lingqu_shmem_pto_endpoint_wait(
    struct lingqu_shmem_pto_endpoint *endpoint,
    uint64_t op_id,
    uint64_t timeout_ms,
    struct lingqu_shmem_pto_completion *completion);

uint64_t lingqu_shmem_pto_endpoint_pending_op(
    const struct lingqu_shmem_pto_endpoint *endpoint);

/* Returns -EBUSY without changing queues while requests remain uncollected or
 * a protocol fault is sticky. Join all calling threads before a final close.
 */
int lingqu_shmem_pto_endpoint_close_checked(
    struct lingqu_shmem_pto_endpoint *endpoint);

/* Compatibility wrapper: a pending endpoint remains owned by the caller. */
void lingqu_shmem_pto_endpoint_close(
    struct lingqu_shmem_pto_endpoint *endpoint);

#ifdef __cplusplus
}
#endif

#endif
