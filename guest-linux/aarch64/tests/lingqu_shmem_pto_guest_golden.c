/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lingqu_shmem_pto_guest.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "check failed line=%d: %s\n", __LINE__,       \
                    #condition);                                             \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void print_hex(const void *data, size_t length)
{
    const uint8_t *bytes = data;
    size_t index;

    for (index = 0; index < length; index++) {
        printf("%02x", bytes[index]);
    }
    putchar('\n');
}

int main(void)
{
    struct lingqu_shmem_pto_memref_desc memrefs[3] = { 0 };
    struct lingqu_shmem_pto_scalar_desc scalar = { 0 };
    struct lingqu_shmem_pto_dispatch_desc dispatch = { 0 };
    struct lingqu_shmem_pto_wire_result result;
    LingquPtoDispatchSlotV2 slot;
    LingquPtoDispatchControlV2 *control;
    LingquShmemMemrefV1 *wire_memrefs;
    LingquPtoScalarV1 *wire_scalars;
    uint8_t metadata[4096];
    uint64_t mapping_ref = 0;
    uint32_t index;
    int rc;

    CHECK(lingqu_shmem_pto_obmm_mapping_ref(3, 7, &mapping_ref) == 0);
    CHECK(mapping_ref == UINT64_C(0x703));
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(0, 7, &mapping_ref) == -ERANGE);
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(64, 7, &mapping_ref) == 0);
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(65, 7, &mapping_ref) == -ERANGE);
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(3, 0, &mapping_ref) == -ERANGE);
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(
              3, LINGQU_PTO_OBMM_MAP_GENERATION_MAX + 1,
              &mapping_ref) == -ERANGE);
    CHECK(lingqu_shmem_pto_obmm_mapping_ref(3, 7, &mapping_ref) == 0);

    for (index = 0; index < 3; index++) {
        memrefs[index].opaque_mapping_ref = mapping_ref;
        memrefs[index].ub_gm_addr = UINT64_C(0x200000);
        memrefs[index].byte_offset = (uint64_t)index * 64;
        memrefs[index].byte_length = 64;
        memrefs[index].arg_index = index;
        memrefs[index].rank = 1;
        memrefs[index].dtype = 0;
        memrefs[index].role = index < 2 ?
            LINGQU_PTO_MEMREF_INPUT : LINGQU_PTO_MEMREF_OUTPUT;
        memrefs[index].access = index < 2 ?
            LINGQU_PTO_UB_GM_READ : LINGQU_PTO_UB_GM_WRITE;
        memrefs[index].shape[0] = 16;
        memrefs[index].strides[0] = 1;
    }
    dispatch.op_id = UINT64_C(0x1122334455667788);
    dispatch.request_id = UINT64_C(0x8877665544332211);
    dispatch.callable_id = 1;
    dispatch.artifact_fingerprint = UINT64_C(0xa1a2a3a4a5a6a7a8);
    dispatch.requester_cna = UINT32_C(0x1234);
    dispatch.memrefs = memrefs;
    dispatch.memref_count = 3;

    CHECK(lingqu_shmem_pto_dispatch_wire_bytes(&dispatch) == 328);
    rc = lingqu_shmem_pto_dispatch_materialize(
        &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
        &slot, &result);
    CHECK(rc == 0);
    CHECK(result.control_table_iova == UINT64_C(0x100000));
    CHECK(result.metadata_bytes == 328);
    CHECK(slot.descriptor_tag == LINGQU_PTO_DISPATCH_SLOT_TAG_V2);
    control = (LingquPtoDispatchControlV2 *)metadata;
    wire_memrefs = (LingquShmemMemrefV1 *)(metadata + sizeof(*control));
    CHECK(control->metadata_crc32 == result.metadata_crc32);
    CHECK(control->memref_table_iova == UINT64_C(0x100040));
    CHECK(control->scalar_table_iova == 0);
    CHECK(wire_memrefs[0].shape_table_iova == UINT64_C(0x100130));
    CHECK(wire_memrefs[2].stride_table_iova == UINT64_C(0x100144));

    scalar.arg_index = 3;
    scalar.dtype = 7;
    scalar.value = UINT64_C(0x0102030405060708);
    dispatch.scalars = &scalar;
    dispatch.scalar_count = 1;
    CHECK(lingqu_shmem_pto_dispatch_wire_bytes(&dispatch) == 352);
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == 0);
    control = (LingquPtoDispatchControlV2 *)metadata;
    wire_scalars = (LingquPtoScalarV1 *)(
        metadata + sizeof(*control) + sizeof(memrefs) / sizeof(memrefs[0]) *
        sizeof(LingquShmemMemrefV1));
    CHECK(result.metadata_bytes == 352);
    CHECK(control->scalar_count == 1);
    CHECK(control->scalar_table_iova == UINT64_C(0x100130));
    CHECK(wire_scalars[0].arg_index == 3);
    CHECK(wire_scalars[0].dtype == 7);
    CHECK(wire_scalars[0].value == scalar.value);
    scalar.arg_index = 2;
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == -EINVAL);
    scalar.arg_index = 3;
    scalar.dtype = LINGQU_PTO_DTYPE_MAX + 1;
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == -EINVAL);
    scalar.dtype = 7;
    dispatch.scalars = NULL;
    dispatch.scalar_count = 0;

    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, 327, UINT64_C(0x100000),
              &slot, &result) == -EINVAL);
    memrefs[1].arg_index = 0;
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == -EINVAL);
    memrefs[1].arg_index = 1;
    memrefs[2].access = LINGQU_PTO_UB_GM_READ;
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == -EINVAL);
    memrefs[2].access = LINGQU_PTO_UB_GM_WRITE;

    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_MAX - 100,
              &slot, &result) == -EINVAL);
    CHECK(lingqu_shmem_pto_dispatch_materialize(
              &dispatch, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result) == 0);

    printf("bytes=%zu crc=%08" PRIx32 " mapping_ref=%016" PRIx64 "\n",
           result.metadata_bytes, result.metadata_crc32, mapping_ref);
    print_hex(metadata, result.metadata_bytes);
    print_hex(&slot, sizeof(slot));
    return 0;
}
