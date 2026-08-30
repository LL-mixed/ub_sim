/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_pto_guest.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Lingqu guest PTO ABI v2 currently requires a little-endian guest"
#endif

#define LINGQU_SHMEM_PTO_MAX_ARGS \
    (LINGQU_PTO_MAX_MEMREFS + LINGQU_PTO_MAX_SCALARS)
#define LINGQU_SHMEM_PTO_OBMM_MAP_SLOTS 64u

static bool checked_add_size(size_t left, size_t right, size_t *result)
{
    if (!result || left > SIZE_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

static bool checked_mul_size(size_t left, size_t right, size_t *result)
{
    if (!result || (left != 0 && right > SIZE_MAX / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

static bool checked_add_u64(uint64_t left, uint64_t right, uint64_t *result)
{
    if (!result || left > UINT64_MAX - right) {
        return false;
    }
    *result = left + right;
    return true;
}

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

static bool role_access_valid(
    const struct lingqu_shmem_pto_memref_desc *memref)
{
    return (memref->role == LINGQU_PTO_MEMREF_INPUT &&
            memref->access == LINGQU_PTO_UB_GM_READ) ||
           (memref->role == LINGQU_PTO_MEMREF_OUTPUT &&
            memref->access == LINGQU_PTO_UB_GM_WRITE) ||
           (memref->role == LINGQU_PTO_MEMREF_INOUT &&
            memref->access == LINGQU_PTO_UB_GM_READ_WRITE);
}

static bool strided_view_valid(
    const struct lingqu_shmem_pto_memref_desc *memref)
{
    struct {
        uint64_t stride;
        uint32_t dimension;
    } active_dims[LINGQU_PTO_MAX_RANK];
    uint64_t extent_elements = 1;
    uint64_t element_bytes;
    uint32_t active_count = 0;
    uint32_t index;

    if (!memref || memref->rank == 0 ||
        memref->rank > LINGQU_PTO_MAX_RANK) {
        return false;
    }
    element_bytes = dtype_bytes(memref->dtype);
    if (element_bytes == 0) {
        return false;
    }
    for (index = 0; index < memref->rank; index++) {
        uint32_t dimension = memref->shape[index];
        uint64_t stride = memref->strides[index];
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
           extent_elements * element_bytes == memref->byte_length;
}

static uint32_t crc32_ieee_update(uint32_t crc,
                                  const uint8_t *bytes,
                                  size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        uint32_t value = crc ^ bytes[index];
        uint32_t bit;

        for (bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^
                    (0xedb88320u & (0u - (value & 1u)));
        }
        crc = value;
    }
    return crc;
}

static void store_u64_le(uint8_t bytes[8], uint64_t value)
{
    size_t index;

    for (index = 0; index < sizeof(value); index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

int lingqu_shmem_pto_obmm_mapping_ref(uint64_t map_id,
                                      uint64_t map_generation,
                                      uint64_t *mapping_ref)
{
    uint64_t encoded;

    if (!mapping_ref) {
        return -EINVAL;
    }
    if (map_id > LINGQU_SHMEM_PTO_OBMM_MAP_SLOTS) {
        return -ERANGE;
    }
    encoded = lingqu_pto_obmm_mapping_ref_encode(map_id, map_generation);
    if (encoded == 0) {
        return -ERANGE;
    }
    *mapping_ref = encoded;
    return 0;
}

size_t lingqu_shmem_pto_dispatch_wire_bytes(
    const struct lingqu_shmem_pto_dispatch_desc *dispatch)
{
    size_t bytes = sizeof(LingquPtoDispatchControlV2);
    size_t table_bytes;
    uint32_t index;

    if (!dispatch || dispatch->memref_count == 0 ||
        dispatch->memref_count > LINGQU_PTO_MAX_MEMREFS ||
        dispatch->scalar_count > LINGQU_PTO_MAX_SCALARS ||
        !dispatch->memrefs ||
        (dispatch->scalar_count != 0 && !dispatch->scalars)) {
        return 0;
    }
    if (!checked_mul_size(dispatch->memref_count,
                          sizeof(LingquShmemMemrefV1), &table_bytes) ||
        !checked_add_size(bytes, table_bytes, &bytes) ||
        !checked_mul_size(dispatch->scalar_count,
                          sizeof(LingquPtoScalarV1), &table_bytes) ||
        !checked_add_size(bytes, table_bytes, &bytes)) {
        return 0;
    }
    for (index = 0; index < dispatch->memref_count; index++) {
        size_t view_bytes;

        if (dispatch->memrefs[index].rank == 0 ||
            dispatch->memrefs[index].rank > LINGQU_PTO_MAX_RANK ||
            !checked_mul_size(dispatch->memrefs[index].rank,
                              2 * sizeof(uint32_t), &view_bytes) ||
            !checked_add_size(bytes, view_bytes, &bytes)) {
            return 0;
        }
    }
    return bytes;
}

static bool dispatch_identity_valid(
    const struct lingqu_shmem_pto_dispatch_desc *dispatch,
    uint32_t total_args)
{
    return dispatch->op_id != 0 && dispatch->request_id != 0 &&
           dispatch->callable_id != 0 &&
           dispatch->artifact_fingerprint != 0 &&
           dispatch->requester_cna != 0 &&
           dispatch->requester_cna <= LINGQU_PTO_CNA_MAX &&
           total_args <= LINGQU_SHMEM_PTO_MAX_ARGS;
}

int lingqu_shmem_pto_dispatch_materialize(
    const struct lingqu_shmem_pto_dispatch_desc *dispatch,
    void *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *result)
{
    bool arg_seen[LINGQU_SHMEM_PTO_MAX_ARGS] = { false };
    LingquPtoDispatchControlV2 *control;
    LingquShmemMemrefV1 *wire_memrefs;
    LingquPtoScalarV1 *wire_scalars;
    uint8_t *wire = metadata;
    uint8_t *view_cursor;
    uint32_t total_args;
    uint32_t crc = 0xffffffffu;
    uint32_t index;
    size_t required;
    size_t offset;
    uint64_t metadata_end;

    if (!dispatch || !metadata || !slot || !result) {
        return -EINVAL;
    }
    required = lingqu_shmem_pto_dispatch_wire_bytes(dispatch);
    total_args = dispatch->memref_count + dispatch->scalar_count;
    if (required == 0 || metadata_capacity < required || metadata_iova == 0 ||
        !checked_add_u64(metadata_iova, required, &metadata_end) ||
        !dispatch_identity_valid(dispatch, total_args)) {
        return -EINVAL;
    }
    (void)metadata_end;

    memset(metadata, 0, required);
    memset(slot, 0, sizeof(*slot));
    memset(result, 0, sizeof(*result));

    control = (LingquPtoDispatchControlV2 *)wire;
    offset = sizeof(*control);
    wire_memrefs = (LingquShmemMemrefV1 *)(wire + offset);
    offset += (size_t)dispatch->memref_count * sizeof(*wire_memrefs);
    wire_scalars = (LingquPtoScalarV1 *)(wire + offset);
    offset += (size_t)dispatch->scalar_count * sizeof(*wire_scalars);
    view_cursor = wire + offset;

    control->abi_version = LINGQU_PTO_DISPATCH_ABI_V2;
    control->struct_bytes = sizeof(*control);
    control->request_id = dispatch->request_id;
    control->callable_id = dispatch->callable_id;
    control->memref_count = dispatch->memref_count;
    control->scalar_count = dispatch->scalar_count;
    control->memref_table_iova = metadata_iova + sizeof(*control);
    control->scalar_table_iova = dispatch->scalar_count == 0 ? 0 :
        control->memref_table_iova +
        (uint64_t)dispatch->memref_count * sizeof(*wire_memrefs);
    control->artifact_fingerprint = dispatch->artifact_fingerprint;
    control->requester_cna = dispatch->requester_cna;

    for (index = 0; index < dispatch->memref_count; index++) {
        const struct lingqu_shmem_pto_memref_desc *source =
            &dispatch->memrefs[index];
        LingquShmemMemrefV1 *target = &wire_memrefs[index];
        uint64_t view_end;
        size_t shape_bytes = (size_t)source->rank * sizeof(uint32_t);

        if (source->opaque_mapping_ref == 0 || source->ub_gm_addr == 0 ||
            source->byte_length == 0 ||
            source->arg_index >= dispatch->memref_count ||
            arg_seen[source->arg_index] || !role_access_valid(source) ||
            !strided_view_valid(source) ||
            !checked_add_u64(source->byte_offset, source->byte_length,
                             &view_end) ||
            !checked_add_u64(source->ub_gm_addr, view_end, &view_end)) {
            return -EINVAL;
        }
        arg_seen[source->arg_index] = true;
        target->abi_version = LINGQU_SHMEM_MEMREF_ABI_V1;
        target->struct_bytes = sizeof(*target);
        target->opaque_mapping_ref = source->opaque_mapping_ref;
        target->ub_gm_addr = source->ub_gm_addr;
        target->byte_offset = source->byte_offset;
        target->byte_length = source->byte_length;
        target->shape_table_iova = metadata_iova +
            (uint64_t)(view_cursor - wire);
        memcpy(view_cursor, source->shape, shape_bytes);
        view_cursor += shape_bytes;
        target->stride_table_iova = metadata_iova +
            (uint64_t)(view_cursor - wire);
        memcpy(view_cursor, source->strides, shape_bytes);
        view_cursor += shape_bytes;
        target->arg_index = source->arg_index;
        target->rank = source->rank;
        target->dtype = source->dtype;
        target->role = source->role;
        target->access = source->access;
    }

    for (index = 0; index < dispatch->scalar_count; index++) {
        const struct lingqu_shmem_pto_scalar_desc *source =
            &dispatch->scalars[index];
        LingquPtoScalarV1 *target = &wire_scalars[index];

        if (source->arg_index < dispatch->memref_count ||
            source->arg_index >= total_args || arg_seen[source->arg_index] ||
            dtype_bytes(source->dtype) == 0) {
            return -EINVAL;
        }
        arg_seen[source->arg_index] = true;
        target->abi_version = LINGQU_PTO_SCALAR_ABI_V1;
        target->struct_bytes = sizeof(*target);
        target->arg_index = source->arg_index;
        target->dtype = source->dtype;
        target->value = source->value;
    }
    for (index = 0; index < total_args; index++) {
        if (!arg_seen[index]) {
            return -EINVAL;
        }
    }
    if ((size_t)(view_cursor - wire) != required) {
        return -EINVAL;
    }

    control->metadata_crc32 = 0;
    crc = crc32_ieee_update(crc, wire, sizeof(*control));
    crc = crc32_ieee_update(
        crc, (const uint8_t *)wire_memrefs,
        (size_t)dispatch->memref_count * sizeof(*wire_memrefs));
    crc = crc32_ieee_update(
        crc, (const uint8_t *)wire_scalars,
        (size_t)dispatch->scalar_count * sizeof(*wire_scalars));
    for (index = 0; index < dispatch->memref_count; index++) {
        size_t shape_bytes =
            (size_t)dispatch->memrefs[index].rank * sizeof(uint32_t);
        const uint8_t *shape = wire +
            (wire_memrefs[index].shape_table_iova - metadata_iova);
        const uint8_t *strides = wire +
            (wire_memrefs[index].stride_table_iova - metadata_iova);

        crc = crc32_ieee_update(crc, shape, shape_bytes);
        crc = crc32_ieee_update(crc, strides, shape_bytes);
    }
    control->metadata_crc32 = crc ^ 0xffffffffu;

    slot->descriptor_tag = LINGQU_PTO_DISPATCH_SLOT_TAG_V2;
    store_u64_le(slot->op_id_le, dispatch->op_id);
    store_u64_le(slot->control_table_iova_le, metadata_iova);
    result->control_table_iova = metadata_iova;
    result->metadata_bytes = required;
    result->metadata_crc32 = control->metadata_crc32;
    return 0;
}
