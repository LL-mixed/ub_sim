/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_GUEST_H
#define LINGQU_SHMEM_PTO_GUEST_H

#include <stddef.h>
#include <stdint.h>

#include <linqu_shmem_pto_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lingqu_shmem_pto_memref_desc {
    uint64_t opaque_mapping_ref;
    uint64_t ub_gm_addr;
    uint64_t byte_offset;
    uint64_t byte_length;
    uint32_t arg_index;
    uint32_t rank;
    uint16_t dtype;
    uint8_t role;
    uint8_t access;
    uint32_t shape[LINGQU_PTO_MAX_RANK];
    uint32_t strides[LINGQU_PTO_MAX_RANK];
};

struct lingqu_shmem_pto_scalar_desc {
    uint32_t arg_index;
    uint16_t dtype;
    uint64_t value;
};

struct lingqu_shmem_pto_dispatch_desc {
    uint64_t op_id;
    uint64_t request_id;
    uint64_t callable_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    const struct lingqu_shmem_pto_memref_desc *memrefs;
    uint32_t memref_count;
    const struct lingqu_shmem_pto_scalar_desc *scalars;
    uint32_t scalar_count;
};

struct lingqu_shmem_pto_wire_result {
    uint64_t control_table_iova;
    size_t metadata_bytes;
    uint32_t metadata_crc32;
};

int lingqu_shmem_pto_obmm_mapping_ref(uint64_t map_id,
                                      uint64_t map_generation,
                                      uint64_t *mapping_ref);

size_t lingqu_shmem_pto_dispatch_wire_bytes(
    const struct lingqu_shmem_pto_dispatch_desc *dispatch);

int lingqu_shmem_pto_dispatch_materialize(
    const struct lingqu_shmem_pto_dispatch_desc *dispatch,
    void *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *result);

#ifdef __cplusplus
}
#endif

#endif
