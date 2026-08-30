/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lingqu_shmem.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_sim.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "check failed line=%d: %s\n", __LINE__,       \
                    #condition);                                             \
            return 1;                                                        \
        }                                                                    \
    } while (0)

int main(void)
{
    struct lingqu_shmem_sim_region_desc region_desc = {
        .mapped_addr = (void *)(uintptr_t)UINT64_C(0x400000),
        .mapped_length = 4096,
        .ub_gm_addr = UINT64_C(0x200000),
        .opaque_mapping_ref = UINT64_C(0x703),
    };
    struct lingqu_shmem_memref_spec spec = {
        .byte_length = 64,
        .rank = 1,
        .dtype = 0,
        .access = LINGQU_SHMEM_ACCESS_READ,
        .shape = { 16 },
        .strides = { 1 },
    };
    struct lingqu_shmem_region *region = NULL;
    struct lingqu_shmem_memref *memrefs[3] = { NULL };
    struct lingqu_shmem_pto_memref_arg args[3] = { 0 };
    struct lingqu_shmem_pto_request request = { 0 };
    struct lingqu_shmem_pto_inflight *inflight = NULL;
    struct lingqu_shmem_pto_wire_result result;
    LingquPtoDispatchSlotV2 slot;
    LingquPtoDispatchControlV2 *control;
    LingquShmemMemrefV1 *wire_memrefs;
    uint8_t metadata[4096];
    uint32_t index;

    CHECK(lingqu_shmem_sim_region_create(&region_desc, &region) == 0);
    CHECK(lingqu_shmem_sim_region_destroy(region) == 0);
    region = NULL;
    CHECK(lingqu_shmem_sim_region_create(&region_desc, &region) == 0);

    spec.byte_offset = 4096;
    CHECK(lingqu_shmem_memref_create(region, &spec, &memrefs[0]) ==
          -EINVAL);
    spec.byte_offset = 0;
    spec.strides[0] = 2;
    CHECK(lingqu_shmem_memref_create(region, &spec, &memrefs[0]) ==
          -EINVAL);
    spec.strides[0] = 1;

    spec.rank = 2;
    spec.shape[0] = 3;
    spec.shape[1] = 5;
    spec.strides[0] = 8;
    spec.strides[1] = 1;
    spec.byte_length = 84;
    CHECK(lingqu_shmem_memref_create(region, &spec, &memrefs[0]) == 0);
    CHECK(lingqu_shmem_memref_destroy(memrefs[0]) == 0);
    memrefs[0] = NULL;

    spec.strides[0] = 1;
    spec.strides[1] = 3;
    spec.byte_length = 60;
    CHECK(lingqu_shmem_memref_create(region, &spec, &memrefs[0]) == 0);
    CHECK(lingqu_shmem_memref_destroy(memrefs[0]) == 0);
    memrefs[0] = NULL;

    spec.strides[0] = 1;
    spec.strides[1] = 2;
    CHECK(lingqu_shmem_memref_create(region, &spec, &memrefs[0]) ==
          -EINVAL);

    spec.rank = 1;
    spec.shape[0] = 16;
    spec.shape[1] = 0;
    spec.strides[0] = 1;
    spec.strides[1] = 0;
    spec.byte_length = 64;

    for (index = 0; index < 3; index++) {
        spec.byte_offset = (uint64_t)index * 64;
        spec.access = index < 2 ? LINGQU_SHMEM_ACCESS_READ :
                                 LINGQU_SHMEM_ACCESS_WRITE;
        CHECK(lingqu_shmem_memref_create(region, &spec,
                                          &memrefs[index]) == 0);
        args[index].memref = memrefs[index];
        args[index].arg_index = index;
    }
    CHECK(lingqu_shmem_sim_region_destroy(region) == -EBUSY);

    request.op_id = UINT64_C(0x1122334455667788);
    request.request_id = UINT64_C(0x8877665544332211);
    request.callable_id = 1;
    request.artifact_fingerprint = UINT64_C(0xa1a2a3a4a5a6a7a8);
    request.requester_cna = UINT32_C(0x1234);
    request.memrefs = args;
    request.memref_count = 3;
    CHECK(lingqu_shmem_pto_dispatch_prepare(
              &request, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &result, &inflight) == 0);
    CHECK(result.metadata_bytes == 328);
    CHECK(lingqu_shmem_memref_destroy(memrefs[0]) == -EBUSY);
    CHECK(lingqu_shmem_sim_region_destroy(region) == -EBUSY);
    control = (LingquPtoDispatchControlV2 *)metadata;
    wire_memrefs = (LingquShmemMemrefV1 *)(metadata + sizeof(*control));
    CHECK(control->memref_count == 3);
    CHECK(wire_memrefs[0].opaque_mapping_ref == UINT64_C(0x703));
    CHECK(wire_memrefs[0].ub_gm_addr == UINT64_C(0x200000));
    CHECK(wire_memrefs[2].byte_offset == 128);
    CHECK(wire_memrefs[2].role == LINGQU_PTO_MEMREF_OUTPUT);

    lingqu_shmem_pto_dispatch_finish(inflight);
    inflight = NULL;
    for (index = 0; index < 3; index++) {
        CHECK(lingqu_shmem_memref_destroy(memrefs[index]) == 0);
        memrefs[index] = NULL;
    }
    CHECK(lingqu_shmem_sim_region_destroy(region) == 0);
    puts("lingqu_shmem_lifetime_golden=pass");
    return 0;
}
