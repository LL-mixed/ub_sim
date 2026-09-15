/* SPDX-License-Identifier: MIT */
#ifndef W5_MANAGED_MODEL_GUEST_H
#define W5_MANAGED_MODEL_GUEST_H

#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "components/mem_service/lingqu_object_service.h"

#define W5_MANAGED_MODEL_CALLABLE_ID UINT64_C(4)
#define W5_MANAGED_MODEL_CONTROL_MAGIC UINT64_C(0x314d474d3557514c)
#define W5_MANAGED_MODEL_PREFIX_BYTES 8192u
#define W5_MANAGED_MODEL_CONTROL_BYTES (64u + 8192u + 512u)
#define W5_MANAGED_MODEL_MAX_VIEW_BYTES (64u * 1024u * 1024u)

/* Encode control into caller-owned storage before publishing its read-only
 * SDK view. References must be the exact results used by SDK acquire/map;
 * this codec cannot grant service access. Only prompt control is copied. */
int w5_managed_model_control_encode(void *buffer, size_t length,
    uint64_t op_id, uint64_t request_id, const uint32_t levels[8],
    const void *prefix, size_t prefix_length,
    const struct lingqu_object_ref_wire_v2 *references, unsigned input_count);

struct w5_managed_model_operation;

/* owners: read-only control, 0..2 read-only SDK reference views, then a
 * separate writable managed output allocation. Each view begins at offset 0;
 * input lengths match their full V2 payload_bytes. All owners share a domain.
 * The caller retains endpoint/context and stable queue metadata until release.
 * Prepare errors may return a cleanup-only operation; release must be retried.
 * This adapter never allocates a legacy arena or selects a transport. */
int w5_managed_model_prepare(struct lingqu_shmem_mem_service_context *context,
    struct mem_service_mapping_owner *const *owners, const uint64_t *lengths,
    unsigned input_count, uint64_t op_id, uint64_t request_id,
    uint64_t fingerprint, uint32_t requester_cna,
    void *metadata, size_t metadata_bytes, uint64_t metadata_iova,
    struct w5_managed_model_operation **operation_out);

/* Submit once, then poll the SAME endpoint/op until a valid completion. A
 * timeout (-EAGAIN to poll), protocol fault or uncertain result retains every
 * lease and metadata pin. A failed completion permits cleanup, not publish. */
int w5_managed_model_submit(struct w5_managed_model_operation *operation,
    struct lingqu_shmem_pto_endpoint *endpoint);
int w5_managed_model_poll(struct w5_managed_model_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);
int w5_managed_model_release(struct w5_managed_model_operation *operation);

#endif
