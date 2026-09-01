/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_MEM_SERVICE_BACKEND_H
#define LINGQU_SHMEM_MEM_SERVICE_BACKEND_H

#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_sim.h"

#include <stdbool.h>
#include <stdint.h>

struct mem_service_object_payload_view;

struct lingqu_shmem_mem_service_region_binding {
    struct lingqu_shmem_sim_region_desc region;
    uint64_t object_byte_offset;
    void *backend_lease;
};

struct lingqu_shmem_mem_service_backend_ops {
    int (*acquire)(
        void *backend_context,
        const struct mem_service_object_payload_view *view,
        struct lingqu_shmem_mem_service_region_binding *binding_out);
    int (*acquire_local)(
        void *backend_context,
        uint64_t bytes,
        uint64_t align,
        struct lingqu_shmem_mem_service_region_binding *binding_out,
        struct lingqu_shmem_mem_service_local_buffer *buffer_out);
    int (*release)(void *backend_context, void *backend_lease);
    void (*destroy)(void *backend_context);
};

int lingqu_shmem_mem_service_context_create_for_backend(
    const struct lingqu_shmem_mem_service_backend_ops *ops,
    void *backend_context,
    bool owns_backend_context,
    struct lingqu_shmem_mem_service_context **context_out);

#endif
