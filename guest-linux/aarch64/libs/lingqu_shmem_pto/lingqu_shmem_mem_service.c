/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_mem_service_backend.h"

#include "components/mem_service/lingqu_object_service.h"
#include "components/mem_service/mem_service.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct lingqu_shmem_mem_service_context {
    const struct lingqu_shmem_mem_service_backend_ops *ops;
    void *backend_context;
    bool owns_backend_context;
    atomic_uint active_leases;
};

struct lingqu_shmem_mem_service_lease {
    struct lingqu_shmem_mem_service_context *context;
    struct lingqu_shmem_region *region;
    struct lingqu_shmem_memref *memref;
    void *backend_lease;
};

static bool object_view_valid(
    const struct mem_service_object_payload_view *view)
{
    const struct lingqu_object_ref_wire *object_ref;

    if (!view || !view->data || view->len == 0) {
        return false;
    }
    object_ref = &view->object_ref;
    return object_ref->magic == LINGQU_OBJECT_REF_MAGIC &&
           object_ref->layout_version == LINGQU_OBJECT_REF_LAYOUT_VERSION &&
           object_ref->state == LINGQU_OBJECT_STATE_COMMITTED_WIRE &&
           object_ref->owner_entity == view->owner_node &&
           object_ref->object_kind == view->payload_kind &&
           object_ref->payload_offset == view->backing_offset &&
           object_ref->payload_bytes == view->len &&
           object_ref->payload_checksum == view->checksum;
}

int lingqu_shmem_mem_service_context_create_for_backend(
    const struct lingqu_shmem_mem_service_backend_ops *ops,
    void *backend_context,
    bool owns_backend_context,
    struct lingqu_shmem_mem_service_context **context_out)
{
    struct lingqu_shmem_mem_service_context *context;

    if (!ops || !ops->acquire || !ops->release || !context_out) {
        return -EINVAL;
    }
    *context_out = NULL;
    context = calloc(1, sizeof(*context));
    if (!context) {
        return -ENOMEM;
    }
    context->ops = ops;
    context->backend_context = backend_context;
    context->owns_backend_context = owns_backend_context;
    atomic_init(&context->active_leases, 0);
    *context_out = context;
    return 0;
}

int lingqu_shmem_mem_service_acquire(
    struct lingqu_shmem_mem_service_context *context,
    const struct mem_service_object_payload_view *view,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_lease **lease_out)
{
    struct lingqu_shmem_mem_service_region_binding binding;
    struct lingqu_shmem_mem_service_lease *lease = NULL;
    struct lingqu_shmem_memref_spec adjusted_spec;
    int rc;

    if (!context || !view || !spec || !memref_out || !lease_out ||
        !object_view_valid(view) || spec->byte_length == 0 ||
        spec->byte_offset > view->len ||
        spec->byte_length > view->len - spec->byte_offset) {
        return -EINVAL;
    }
    *memref_out = NULL;
    *lease_out = NULL;
    memset(&binding, 0, sizeof(binding));
    rc = context->ops->acquire(context->backend_context, view, &binding);
    if (rc != 0) {
        return rc;
    }
    if (!binding.region.mapped_addr || binding.region.mapped_length == 0 ||
        binding.region.ub_gm_addr == 0 ||
        binding.region.opaque_mapping_ref == 0 ||
        binding.object_byte_offset > binding.region.mapped_length ||
        view->len > binding.region.mapped_length - binding.object_byte_offset ||
        binding.object_byte_offset > UINT64_MAX - spec->byte_offset) {
        rc = -EINVAL;
        goto release_backend;
    }
    adjusted_spec = *spec;
    adjusted_spec.byte_offset = binding.object_byte_offset + spec->byte_offset;
    lease = calloc(1, sizeof(*lease));
    if (!lease) {
        rc = -ENOMEM;
        goto release_backend;
    }
    rc = lingqu_shmem_sim_region_create(&binding.region, &lease->region);
    if (rc != 0) {
        goto free_lease;
    }
    rc = lingqu_shmem_memref_create(
        lease->region, &adjusted_spec, &lease->memref);
    if (rc != 0) {
        goto destroy_region;
    }
    lease->context = context;
    lease->backend_lease = binding.backend_lease;
    atomic_fetch_add_explicit(
        &context->active_leases, 1, memory_order_acq_rel);
    *memref_out = lease->memref;
    *lease_out = lease;
    return 0;

destroy_region:
    (void)lingqu_shmem_sim_region_destroy(lease->region);
free_lease:
    free(lease);
release_backend:
    (void)context->ops->release(
        context->backend_context, binding.backend_lease);
    return rc;
}

static int acquire_local_buffer(
    struct lingqu_shmem_mem_service_context *context,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out,
    struct lingqu_shmem_mem_service_lease **lease_out,
    bool model_kv)
{
    struct lingqu_shmem_mem_service_region_binding binding;
    struct lingqu_shmem_mem_service_local_buffer buffer;
    struct lingqu_shmem_mem_service_lease *lease = NULL;
    struct lingqu_shmem_memref_spec adjusted_spec;
    int rc;

    if (!context || !spec || !memref_out ||
        !buffer_out || !lease_out || spec->byte_length == 0 ||
        spec->byte_offset != 0) {
        return -EINVAL;
    }
    *memref_out = NULL;
    *lease_out = NULL;
    memset(buffer_out, 0, sizeof(*buffer_out));
    if (model_kv ? !context->ops->acquire_local_kv : !context->ops->acquire_local) {
        return -EOPNOTSUPP;
    }
    memset(&binding, 0, sizeof(binding));
    memset(&buffer, 0, sizeof(buffer));
    rc = (model_kv ? context->ops->acquire_local_kv : context->ops->acquire_local)(
                                     context->backend_context,
                                     spec->byte_length,
                                     64,
                                     &binding,
                                     &buffer);
    if (rc != 0) {
        return rc;
    }
    if (!binding.region.mapped_addr || binding.region.mapped_length == 0 ||
        binding.region.ub_gm_addr == 0 ||
        binding.region.opaque_mapping_ref == 0 ||
        !buffer.data || buffer.len != spec->byte_length ||
        buffer.backing_offset != binding.object_byte_offset ||
        buffer.backing_offset > binding.region.mapped_length ||
        buffer.len > binding.region.mapped_length - buffer.backing_offset) {
        rc = -EINVAL;
        goto release_backend;
    }
    adjusted_spec = *spec;
    adjusted_spec.byte_offset = binding.object_byte_offset;
    lease = calloc(1, sizeof(*lease));
    if (!lease) {
        rc = -ENOMEM;
        goto release_backend;
    }
    rc = lingqu_shmem_sim_region_create(&binding.region, &lease->region);
    if (rc != 0) {
        goto free_lease;
    }
    rc = lingqu_shmem_memref_create(
        lease->region, &adjusted_spec, &lease->memref);
    if (rc != 0) {
        goto destroy_region;
    }
    lease->context = context;
    lease->backend_lease = binding.backend_lease;
    atomic_fetch_add_explicit(
        &context->active_leases, 1, memory_order_acq_rel);
    *memref_out = lease->memref;
    *buffer_out = buffer;
    *lease_out = lease;
    return 0;

destroy_region:
    (void)lingqu_shmem_sim_region_destroy(lease->region);
free_lease:
    free(lease);
release_backend:
    (void)context->ops->release(
        context->backend_context, binding.backend_lease);
    return rc;
}

int lingqu_shmem_mem_service_acquire_local(
    struct lingqu_shmem_mem_service_context *context,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out,
    struct lingqu_shmem_mem_service_lease **lease_out)
{
    return acquire_local_buffer(context, spec, memref_out, buffer_out, lease_out, false);
}

int lingqu_shmem_mem_service_acquire_local_kv(
    struct lingqu_shmem_mem_service_context *context,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out,
    struct lingqu_shmem_mem_service_lease **lease_out)
{
    return acquire_local_buffer(context, spec, memref_out, buffer_out, lease_out, true);
}

int lingqu_shmem_mem_service_release(
    struct lingqu_shmem_mem_service_lease *lease)
{
    struct lingqu_shmem_mem_service_context *context;
    int backend_rc;
    int rc;

    if (!lease || !lease->context || !lease->memref || !lease->region) {
        return -EINVAL;
    }
    context = lease->context;
    rc = lingqu_shmem_memref_destroy(lease->memref);
    if (rc != 0) {
        return rc;
    }
    lease->memref = NULL;
    rc = lingqu_shmem_sim_region_destroy(lease->region);
    if (rc != 0) {
        return rc;
    }
    lease->region = NULL;
    backend_rc = context->ops->release(
        context->backend_context, lease->backend_lease);
    lease->backend_lease = NULL;
    atomic_fetch_sub_explicit(
        &context->active_leases, 1, memory_order_acq_rel);
    memset(lease, 0, sizeof(*lease));
    free(lease);
    return backend_rc;
}

int lingqu_shmem_mem_service_close(
    struct lingqu_shmem_mem_service_context *context)
{
    if (!context) {
        return -EINVAL;
    }
    if (atomic_load_explicit(
            &context->active_leases, memory_order_acquire) != 0) {
        return -EBUSY;
    }
    if (context->owns_backend_context && context->ops->destroy) {
        context->ops->destroy(context->backend_context);
    }
    memset(context, 0, sizeof(*context));
    free(context);
    return 0;
}

uint32_t lingqu_shmem_mem_service_active_leases(
    const struct lingqu_shmem_mem_service_context *context)
{
    if (!context) {
        return 0;
    }
    return atomic_load_explicit(
        &context->active_leases, memory_order_acquire);
}
