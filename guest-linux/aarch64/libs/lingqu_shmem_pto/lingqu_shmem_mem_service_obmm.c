/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_mem_service_backend.h"
#include "lingqu_shmem_pto_guest.h"

#include "components/mem_service/mem_service.h"
#include "components/mem_service/mem_service_cluster_runtime.h"
#include "components/mem_service/mem_service_obmm_objects.h"
#include "libs/obmm_async/obmm_async.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct lingqu_shmem_mem_service_obmm_context {
    struct obmm_async *async_runtime;
    struct mem_service_cluster_runtime *local_alias_runtime;
    struct obmm_helpers_region local_alias_region;
    uint64_t local_alias_mem_id;
    uint64_t local_alias_pa;
};

struct lingqu_shmem_mem_service_obmm_lease {
    struct obmm_async_map map;
};

static int obmm_compute_ensure_local_alias(
    struct lingqu_shmem_mem_service_obmm_context *context,
    struct mem_service_cluster_runtime *runtime)
{
    struct obmm_helpers_meta meta;
    uint64_t import_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    int alias_index;

    if (!context || !runtime || runtime->local_idx < 0 ||
        runtime->local_idx >= runtime->node_count || runtime->node_count <= 0 ||
        runtime->node_count > OBMM_POOL_HELPERS_MAX_NODES) {
        return -EINVAL;
    }
    if (context->local_alias_mem_id != 0) {
        return context->local_alias_runtime == runtime ? 0 : -EBUSY;
    }
    if (!obmm_alloc_import_pas(
            runtime->node_count, runtime->region_size, import_pas,
            import_osync, obmm_parse_import_cache_mode())) {
        return -ENOSPC;
    }
    alias_index = runtime->node_count - 1;
    memset(&meta, 0, sizeof(meta));
    meta.export_mem_id = runtime->metas[runtime->local_idx].export_mem_id;
    meta.remote_uba = runtime->metas[runtime->local_idx].remote_uba;
    meta.size = runtime->metas[runtime->local_idx].size;
    meta.token_id = runtime->metas[runtime->local_idx].token_id;
    meta.export_cna = runtime->metas[runtime->local_idx].export_cna;
    if (meta.export_mem_id == 0 || meta.remote_uba == 0 ||
        meta.size != runtime->region_size || meta.token_id == 0 ||
        meta.export_cna != runtime->local_cna) {
        return -EINVAL;
    }
    if (obmm_do_import(
            runtime->obmm_fd, &meta, runtime->local_cna,
            import_pas[alias_index], meta.token_id,
            &context->local_alias_mem_id) != 0) {
        return errno != 0 ? -errno : -EIO;
    }
    if (obmm_map_region(
            context->local_alias_mem_id, runtime->region_size, true,
            &context->local_alias_region) != 0) {
        int saved_errno = errno;

        (void)obmm_do_unimport(
            runtime->obmm_fd, context->local_alias_mem_id);
        context->local_alias_mem_id = 0;
        return saved_errno != 0 ? -saved_errno : -EIO;
    }
    context->local_alias_runtime = runtime;
    context->local_alias_pa = import_pas[alias_index];
    return 0;
}

static int obmm_compute_bind_slot(
    struct lingqu_shmem_mem_service_obmm_context *context,
    struct mem_service_cluster_runtime *runtime,
    struct mem_service_cluster_slot *slot,
    uint64_t object_byte_offset,
    struct lingqu_shmem_mem_service_region_binding *binding_out)
{
    struct lingqu_shmem_mem_service_obmm_lease *lease = NULL;
    void *mapped_addr;
    uint64_t mapped_length;
    uint64_t mapped_mem_id;
    uint64_t fallback_local_pa;
    uint64_t mapping_ref = 0;
    uint64_t ub_gm_addr = 0;
    int rc;

    if (!context || !context->async_runtime || !runtime || !slot ||
        !slot->region.addr || slot->region.len == 0 || slot->mem_id == 0 ||
        object_byte_offset > slot->region.len || !binding_out) {
        return -EINVAL;
    }
    mapped_addr = slot->region.addr;
    mapped_length = slot->region.len;
    mapped_mem_id = slot->mem_id;
    fallback_local_pa = slot->local_pa;
    if (slot->is_local) {
        rc = obmm_compute_ensure_local_alias(context, runtime);
        if (rc != 0) {
            return rc;
        }
        if (runtime->payload_offset >= runtime->region_size ||
            runtime->payload_offset > UINT64_MAX - context->local_alias_pa) {
            return -EOVERFLOW;
        }
        mapped_addr = (uint8_t *)context->local_alias_region.addr +
                      runtime->payload_offset;
        mapped_length = runtime->region_size - runtime->payload_offset;
        mapped_mem_id = context->local_alias_mem_id;
        fallback_local_pa = context->local_alias_pa + runtime->payload_offset;
    } else if (runtime->payload_offset > UINT64_MAX - fallback_local_pa) {
        return -EOVERFLOW;
    } else {
        fallback_local_pa += runtime->payload_offset;
    }
    lease = calloc(1, sizeof(*lease));
    if (!lease) {
        return -ENOMEM;
    }
    rc = obmm_async_map_register(
        context->async_runtime, runtime->obmm_fd, mapped_mem_id,
        mapped_addr, mapped_length, &lease->map);
    if (rc != 0) {
        free(lease);
        return rc;
    }
    rc = lingqu_shmem_pto_obmm_mapping_ref(
        lease->map.id, lease->map.generation, &mapping_ref);
    if (rc != 0) {
        (void)obmm_async_map_unregister(context->async_runtime, &lease->map);
        free(lease);
        return rc;
    }
    ub_gm_addr = lease->map.local_pa;
    if (ub_gm_addr == 0 && fallback_local_pa != 0) {
        ub_gm_addr = fallback_local_pa;
        rc = 0;
    } else if (ub_gm_addr == 0) {
        rc = lingqu_shmem_sim_phys_for_virt(
            mapped_addr, &ub_gm_addr);
    } else {
        rc = 0;
    }
    if (rc != 0 || ub_gm_addr == 0) {
        (void)obmm_async_map_unregister(
            context->async_runtime, &lease->map);
        free(lease);
        return rc != 0 ? rc : -EFAULT;
    }
    memset(binding_out, 0, sizeof(*binding_out));
    binding_out->region = (struct lingqu_shmem_sim_region_desc) {
        .mapped_addr = mapped_addr,
        .mapped_length = mapped_length,
        .ub_gm_addr = ub_gm_addr,
        .opaque_mapping_ref = mapping_ref,
    };
    binding_out->object_byte_offset = object_byte_offset;
    binding_out->backend_lease = lease;
    return 0;
}

static int obmm_compute_acquire(
    void *backend_context,
    const struct mem_service_object_payload_view *view,
    struct lingqu_shmem_mem_service_region_binding *binding_out)
{
    struct lingqu_shmem_mem_service_obmm_context *context = backend_context;
    struct mem_service_cluster_runtime *runtime;
    struct mem_service_cluster_slot *slot;

    if (!context || !context->async_runtime || !view || !binding_out) {
        return -EINVAL;
    }
    runtime = mem_service_cluster_runtime_current();
    if (!runtime || !runtime->active || view->owner_node >= (uint32_t)runtime->node_count) {
        return -ENODEV;
    }
    slot = &runtime->slots[view->owner_node];
    if (!slot->region.addr || slot->region.len == 0 || slot->mem_id == 0 ||
        view->backing_offset > slot->region.len ||
        view->len > slot->region.len - view->backing_offset ||
        view->data != (const uint8_t *)slot->region.addr + view->backing_offset) {
        return -EOPNOTSUPP;
    }
    return obmm_compute_bind_slot(
        context, runtime, slot, view->backing_offset, binding_out);
}

static int obmm_compute_acquire_local(
    void *backend_context,
    uint64_t bytes,
    uint64_t align,
    struct lingqu_shmem_mem_service_region_binding *binding_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out)
{
    struct lingqu_shmem_mem_service_obmm_context *context = backend_context;
    struct mem_service_cluster_runtime *runtime;
    struct mem_service_cluster_slot *slot;
    uint64_t offset = 0;
    int rc;

    if (!context || !context->async_runtime || bytes == 0 || !binding_out ||
        !buffer_out) {
        return -EINVAL;
    }
    runtime = mem_service_cluster_runtime_current();
    if (!runtime || !runtime->active || runtime->local_idx < 0 ||
        runtime->local_idx >= runtime->node_count) {
        return -ENODEV;
    }
    slot = &runtime->slots[runtime->local_idx];
    if (!slot->is_local || !slot->region.addr || slot->region.len == 0 ||
        slot->mem_id == 0 ||
        mem_service_payload_arena_alloc(runtime, bytes, align, &offset) != 0) {
        return -ENOSPC;
    }
    rc = obmm_compute_bind_slot(context, runtime, slot, offset, binding_out);
    if (rc != 0) {
        return rc;
    }
    memset((uint8_t *)slot->region.addr + offset, 0, bytes);
    *buffer_out = (struct lingqu_shmem_mem_service_local_buffer) {
        .data = (uint8_t *)slot->region.addr + offset,
        .len = bytes,
        .backing_offset = offset,
        .owner_node = (uint32_t)runtime->local_idx,
    };
    return 0;
}

static int obmm_compute_release(void *backend_context, void *backend_lease)
{
    struct lingqu_shmem_mem_service_obmm_context *context = backend_context;
    struct lingqu_shmem_mem_service_obmm_lease *lease = backend_lease;
    int rc;

    if (!context || !context->async_runtime || !lease || lease->map.id == 0) {
        return -EINVAL;
    }
    rc = obmm_async_map_unregister(context->async_runtime, &lease->map);
    memset(lease, 0, sizeof(*lease));
    free(lease);
    return rc;
}

static void obmm_compute_destroy(void *backend_context)
{
    struct lingqu_shmem_mem_service_obmm_context *context = backend_context;

    if (!context) {
        return;
    }
    obmm_async_close(context->async_runtime);
    if (context->local_alias_region.addr ||
        context->local_alias_region.fd >= 0) {
        obmm_unmap_region(&context->local_alias_region);
    }
    if (context->local_alias_mem_id != 0 && context->local_alias_runtime) {
        (void)obmm_do_unimport(
            context->local_alias_runtime->obmm_fd,
            context->local_alias_mem_id);
    }
    memset(context, 0, sizeof(*context));
    free(context);
}

static const struct lingqu_shmem_mem_service_backend_ops obmm_compute_ops = {
    .acquire = obmm_compute_acquire,
    .acquire_local = obmm_compute_acquire_local,
    .release = obmm_compute_release,
    .destroy = obmm_compute_destroy,
};

int lingqu_shmem_mem_service_open(
    struct lingqu_shmem_mem_service_context **context_out)
{
    struct lingqu_shmem_mem_service_obmm_context *backend_context;
    struct obmm_async_options options = {
        .device_path = OBMM_ASYNC_DEFAULT_DEVICE,
        .mode = OBMM_ASYNC_MODE_POLL,
    };
    int rc;

    if (!context_out) {
        return -EINVAL;
    }
    *context_out = NULL;
    backend_context = calloc(1, sizeof(*backend_context));
    if (!backend_context) {
        return -ENOMEM;
    }
    backend_context->local_alias_region.fd = -1;
    rc = obmm_async_open(&backend_context->async_runtime, &options);
    if (rc != 0) {
        free(backend_context);
        return rc;
    }
    rc = lingqu_shmem_mem_service_context_create_for_backend(
        &obmm_compute_ops, backend_context, true, context_out);
    if (rc != 0) {
        obmm_compute_destroy(backend_context);
    }
    return rc;
}
