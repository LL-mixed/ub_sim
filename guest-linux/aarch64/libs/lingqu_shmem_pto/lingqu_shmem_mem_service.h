/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_MEM_SERVICE_H
#define LINGQU_SHMEM_MEM_SERVICE_H

#include "lingqu_shmem.h"

#ifdef __cplusplus
extern "C" {
#endif

struct mem_service_object_payload_view;
struct lingqu_shmem_mem_service_context;
struct lingqu_shmem_mem_service_lease;

struct lingqu_shmem_mem_service_local_buffer {
    uint8_t *data;
    uint64_t len;
    uint64_t backing_offset;
    uint32_t owner_node;
};

/*
 * Open the current Memory Service compute adapter.  The adapter selects the
 * active data-plane provider internally; model code never supplies a provider
 * name or provider descriptor.
 */
int lingqu_shmem_mem_service_open(
    struct lingqu_shmem_mem_service_context **context_out);

/*
 * Acquire a dispatch-bound memref for one committed Memory Service object.
 * spec->byte_offset is relative to the beginning of the object payload.
 */
int lingqu_shmem_mem_service_acquire(
    struct lingqu_shmem_mem_service_context *context,
    const struct mem_service_object_payload_view *view,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_lease **lease_out);

/*
 * Reserve a local Memory Service payload-arena range and expose it as a PTO
 * memref. The caller may commit this exact range through an in-place publish
 * after the dispatch completes.
 */
int lingqu_shmem_mem_service_acquire_local(
    struct lingqu_shmem_mem_service_context *context,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out,
    struct lingqu_shmem_mem_service_lease **lease_out);

/* Reserve a complete tiered model-KV block span for in-place publication.
 * The returned memref covers payload bytes; allocator padding stays private.
 * A backend without this allocation contract returns -EOPNOTSUPP.
 */
int lingqu_shmem_mem_service_acquire_local_kv(
    struct lingqu_shmem_mem_service_context *context,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out,
    struct lingqu_shmem_mem_service_lease **lease_out);

/* Release after the PTO completion for every dispatch using the memref. */
int lingqu_shmem_mem_service_release(
    struct lingqu_shmem_mem_service_lease *lease);

/* Returns -EBUSY while a compute lease remains active. */
int lingqu_shmem_mem_service_close(
    struct lingqu_shmem_mem_service_context *context);

uint32_t lingqu_shmem_mem_service_active_leases(
    const struct lingqu_shmem_mem_service_context *context);

#ifdef __cplusplus
}
#endif

#endif
