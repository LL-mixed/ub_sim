/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include "lingqu_shmem_sim.h"
#include "lingqu_shmem_pto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LINGQU_SHMEM_PAGE_BYTES 4096u

_Static_assert(LINGQU_SHMEM_MAX_RANK == LINGQU_PTO_MAX_RANK,
               "public and wire rank limits must match");
_Static_assert((unsigned int)LINGQU_SHMEM_ACCESS_READ ==
                   (unsigned int)LINGQU_PTO_UB_GM_READ,
               "public and wire read access must match");
_Static_assert((unsigned int)LINGQU_SHMEM_ACCESS_WRITE ==
                   (unsigned int)LINGQU_PTO_UB_GM_WRITE,
               "public and wire write access must match");
_Static_assert((unsigned int)LINGQU_SHMEM_ACCESS_READ_WRITE ==
                   (unsigned int)LINGQU_PTO_UB_GM_READ_WRITE,
               "public and wire read-write access must match");

struct lingqu_shmem_region {
    void *mapped_addr;
    uint64_t mapped_length;
    uint64_t ub_gm_addr;
    uint64_t opaque_mapping_ref;
    atomic_uint memref_count;
    atomic_uint inflight_count;
};

struct lingqu_shmem_memref {
    struct lingqu_shmem_region *region;
    struct lingqu_shmem_memref_spec spec;
    atomic_uint inflight_count;
};

struct lingqu_shmem_pto_inflight {
    struct lingqu_shmem_memref **memrefs;
    uint32_t memref_count;
};

static uint64_t dtype_bytes(uint16_t dtype)
{
    switch (dtype) {
    case 0:
    case 2:
    case 10:
        return 4;
    case 1:
    case 3:
    case 6:
    case 9:
        return 2;
    case 4:
    case 5:
    case 11:
    case 12:
    case 13:
    case 14:
        return 1;
    case 7:
    case 8:
        return 8;
    default:
        return 0;
    }
}

static bool access_valid(uint8_t access)
{
    return access == LINGQU_SHMEM_ACCESS_READ ||
           access == LINGQU_SHMEM_ACCESS_WRITE ||
           access == LINGQU_SHMEM_ACCESS_READ_WRITE;
}

static bool strided_spec_valid(
    const struct lingqu_shmem_memref_spec *spec)
{
    struct {
        uint64_t stride;
        uint32_t dimension;
    } active_dims[LINGQU_SHMEM_MAX_RANK];
    uint64_t extent_elements = 1;
    uint64_t element_bytes;
    uint32_t active_count = 0;
    uint32_t index;

    if (!spec || spec->rank == 0 || spec->rank > LINGQU_SHMEM_MAX_RANK ||
        spec->byte_length == 0 || !access_valid(spec->access)) {
        return false;
    }
    element_bytes = dtype_bytes(spec->dtype);
    if (element_bytes == 0) {
        return false;
    }
    for (index = 0; index < spec->rank; index++) {
        uint32_t dimension = spec->shape[index];
        uint64_t stride = spec->strides[index];
        uint32_t insert_at;

        if (dimension == 0 || stride == 0) {
            return false;
        }
        if (dimension == 1) {
            continue;
        }
        insert_at = active_count;
        while (insert_at > 0 &&
               active_dims[insert_at - 1].stride > stride) {
            active_dims[insert_at] = active_dims[insert_at - 1];
            insert_at--;
        }
        active_dims[insert_at].stride = stride;
        active_dims[insert_at].dimension = dimension;
        active_count++;
    }
    for (index = 0; index < active_count; index++) {
        uint64_t stride = active_dims[index].stride;
        uint64_t dimension_span =
            (uint64_t)active_dims[index].dimension - 1;

        if (stride < extent_elements ||
            dimension_span >
                (UINT64_MAX - extent_elements) / stride) {
            return false;
        }
        extent_elements += dimension_span * stride;
    }
    return extent_elements <= UINT64_MAX / element_bytes &&
           extent_elements * element_bytes == spec->byte_length;
}

static uint8_t role_from_access(uint8_t access)
{
    switch (access) {
    case LINGQU_SHMEM_ACCESS_READ:
        return LINGQU_PTO_MEMREF_INPUT;
    case LINGQU_SHMEM_ACCESS_WRITE:
        return LINGQU_PTO_MEMREF_OUTPUT;
    case LINGQU_SHMEM_ACCESS_READ_WRITE:
        return LINGQU_PTO_MEMREF_INOUT;
    default:
        return 0;
    }
}

int lingqu_shmem_sim_region_create(
    const struct lingqu_shmem_sim_region_desc *desc,
    struct lingqu_shmem_region **region_out)
{
    struct lingqu_shmem_region *region;

    if (!desc || !region_out || !desc->mapped_addr ||
        desc->mapped_length == 0 || desc->ub_gm_addr == 0 ||
        desc->opaque_mapping_ref == 0 ||
        desc->ub_gm_addr > UINT64_MAX - desc->mapped_length) {
        return -EINVAL;
    }
    *region_out = NULL;
    region = calloc(1, sizeof(*region));
    if (!region) {
        return -ENOMEM;
    }
    region->mapped_addr = desc->mapped_addr;
    region->mapped_length = desc->mapped_length;
    region->ub_gm_addr = desc->ub_gm_addr;
    region->opaque_mapping_ref = desc->opaque_mapping_ref;
    atomic_init(&region->memref_count, 0);
    atomic_init(&region->inflight_count, 0);
    *region_out = region;
    return 0;
}

int lingqu_shmem_sim_region_destroy(struct lingqu_shmem_region *region)
{
    if (!region) {
        return -EINVAL;
    }
    if (atomic_load_explicit(&region->memref_count,
                             memory_order_acquire) != 0 ||
        atomic_load_explicit(&region->inflight_count,
                             memory_order_acquire) != 0) {
        return -EBUSY;
    }
    memset(region, 0, sizeof(*region));
    free(region);
    return 0;
}

int lingqu_shmem_sim_phys_for_virt(void *address, uint64_t *physical_out)
{
    uint64_t virtual_address = (uint64_t)(uintptr_t)address;
    uint64_t page_index = virtual_address / LINGQU_SHMEM_PAGE_BYTES;
    uint64_t page_offset = virtual_address % LINGQU_SHMEM_PAGE_BYTES;
    uint64_t entry = 0;
    int fd;
    ssize_t bytes;

    if (!address || !physical_out) {
        return -EINVAL;
    }
    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    bytes = pread(fd, &entry, sizeof(entry),
                  (off_t)(page_index * sizeof(entry)));
    close(fd);
    if (bytes != (ssize_t)sizeof(entry) ||
        (entry & (UINT64_C(1) << 63)) == 0) {
        return -EFAULT;
    }
    entry &= (UINT64_C(1) << 55) - 1;
    if (entry == 0) {
        return -EFAULT;
    }
    *physical_out = entry * LINGQU_SHMEM_PAGE_BYTES + page_offset;
    return 0;
}

int lingqu_shmem_memref_create(
    struct lingqu_shmem_region *region,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out)
{
    struct lingqu_shmem_memref *memref;

    if (!region || !spec || !memref_out ||
        !strided_spec_valid(spec) ||
        spec->byte_offset > region->mapped_length ||
        spec->byte_length > region->mapped_length - spec->byte_offset ||
        region->ub_gm_addr > UINT64_MAX - spec->byte_offset ||
        region->ub_gm_addr + spec->byte_offset >
            UINT64_MAX - spec->byte_length) {
        return -EINVAL;
    }
    *memref_out = NULL;
    memref = calloc(1, sizeof(*memref));
    if (!memref) {
        return -ENOMEM;
    }
    memref->region = region;
    memref->spec = *spec;
    atomic_init(&memref->inflight_count, 0);
    atomic_fetch_add_explicit(&region->memref_count, 1,
                              memory_order_release);
    *memref_out = memref;
    return 0;
}

int lingqu_shmem_memref_destroy(struct lingqu_shmem_memref *memref)
{
    struct lingqu_shmem_region *region;

    if (!memref || !memref->region) {
        return -EINVAL;
    }
    if (atomic_load_explicit(&memref->inflight_count,
                             memory_order_acquire) != 0) {
        return -EBUSY;
    }
    region = memref->region;
    memref->region = NULL;
    atomic_fetch_sub_explicit(&region->memref_count, 1,
                              memory_order_release);
    memset(memref, 0, sizeof(*memref));
    free(memref);
    return 0;
}

static int acquire_memrefs(
    const struct lingqu_shmem_pto_request *request,
    struct lingqu_shmem_pto_inflight **inflight_out,
    struct lingqu_shmem_pto_memref_desc **wire_memrefs_out)
{
    struct lingqu_shmem_pto_inflight *inflight;
    struct lingqu_shmem_pto_memref_desc *wire_memrefs;
    uint32_t index;

    inflight = calloc(1, sizeof(*inflight));
    wire_memrefs = calloc(request->memref_count, sizeof(*wire_memrefs));
    if (!inflight || !wire_memrefs) {
        free(wire_memrefs);
        free(inflight);
        return -ENOMEM;
    }
    inflight->memrefs = calloc(request->memref_count,
                               sizeof(*inflight->memrefs));
    if (!inflight->memrefs) {
        free(wire_memrefs);
        free(inflight);
        return -ENOMEM;
    }
    inflight->memref_count = request->memref_count;
    for (index = 0; index < request->memref_count; index++) {
        struct lingqu_shmem_memref *memref =
            request->memrefs[index].memref;
        struct lingqu_shmem_region *region;

        if (!memref || !memref->region) {
            lingqu_shmem_pto_dispatch_finish(inflight);
            free(wire_memrefs);
            return -EINVAL;
        }
        region = memref->region;
        atomic_fetch_add_explicit(&memref->inflight_count, 1,
                                  memory_order_acq_rel);
        atomic_fetch_add_explicit(&region->inflight_count, 1,
                                  memory_order_acq_rel);
        inflight->memrefs[index] = memref;
        wire_memrefs[index] = (struct lingqu_shmem_pto_memref_desc) {
            .opaque_mapping_ref = region->opaque_mapping_ref,
            .ub_gm_addr = region->ub_gm_addr,
            .byte_offset = memref->spec.byte_offset,
            .byte_length = memref->spec.byte_length,
            .arg_index = request->memrefs[index].arg_index,
            .rank = memref->spec.rank,
            .dtype = memref->spec.dtype,
            .role = role_from_access(memref->spec.access),
            .access = memref->spec.access,
        };
        memcpy(wire_memrefs[index].shape, memref->spec.shape,
               sizeof(wire_memrefs[index].shape));
        memcpy(wire_memrefs[index].strides, memref->spec.strides,
               sizeof(wire_memrefs[index].strides));
    }
    *inflight_out = inflight;
    *wire_memrefs_out = wire_memrefs;
    return 0;
}

int lingqu_shmem_pto_dispatch_prepare(
    const struct lingqu_shmem_pto_request *request,
    void *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *wire_result,
    struct lingqu_shmem_pto_inflight **inflight_out)
{
    struct lingqu_shmem_pto_dispatch_desc dispatch;
    struct lingqu_shmem_pto_inflight *inflight = NULL;
    struct lingqu_shmem_pto_memref_desc *wire_memrefs = NULL;
    int rc;

    if (!request || !metadata || !slot || !wire_result || !inflight_out ||
        request->memref_count == 0 || !request->memrefs) {
        return -EINVAL;
    }
    *inflight_out = NULL;
    rc = acquire_memrefs(request, &inflight, &wire_memrefs);
    if (rc != 0) {
        return rc;
    }
    dispatch = (struct lingqu_shmem_pto_dispatch_desc) {
        .op_id = request->op_id,
        .request_id = request->request_id,
        .callable_id = request->callable_id,
        .artifact_fingerprint = request->artifact_fingerprint,
        .requester_cna = request->requester_cna,
        .memrefs = wire_memrefs,
        .memref_count = request->memref_count,
        .scalars = request->scalars,
        .scalar_count = request->scalar_count,
    };
    rc = lingqu_shmem_pto_dispatch_materialize(
        &dispatch, metadata, metadata_capacity, metadata_iova,
        slot, wire_result);
    free(wire_memrefs);
    if (rc != 0) {
        lingqu_shmem_pto_dispatch_finish(inflight);
        return rc;
    }
    *inflight_out = inflight;
    return 0;
}

void lingqu_shmem_pto_dispatch_finish(
    struct lingqu_shmem_pto_inflight *inflight)
{
    uint32_t index;

    if (!inflight) {
        return;
    }
    for (index = 0; index < inflight->memref_count; index++) {
        struct lingqu_shmem_memref *memref = inflight->memrefs[index];

        if (!memref || !memref->region) {
            continue;
        }
        atomic_fetch_sub_explicit(&memref->region->inflight_count, 1,
                                  memory_order_release);
        atomic_fetch_sub_explicit(&memref->inflight_count, 1,
                                  memory_order_release);
    }
    free(inflight->memrefs);
    memset(inflight, 0, sizeof(*inflight));
    free(inflight);
}
