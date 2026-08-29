/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_H
#define LINGQU_SHMEM_PTO_H

#include <stddef.h>
#include <stdint.h>

#include "lingqu_shmem.h"
#include "lingqu_shmem_pto_guest.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lingqu_shmem_pto_inflight;

struct lingqu_shmem_pto_memref_arg {
    struct lingqu_shmem_memref *memref;
    uint32_t arg_index;
};

struct lingqu_shmem_pto_request {
    uint64_t op_id;
    uint64_t request_id;
    uint64_t callable_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    const struct lingqu_shmem_pto_memref_arg *memrefs;
    uint32_t memref_count;
    const struct lingqu_shmem_pto_scalar_desc *scalars;
    uint32_t scalar_count;
};

int lingqu_shmem_pto_dispatch_prepare(
    const struct lingqu_shmem_pto_request *request,
    void *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *wire_result,
    struct lingqu_shmem_pto_inflight **inflight_out);

void lingqu_shmem_pto_dispatch_finish(
    struct lingqu_shmem_pto_inflight *inflight);

#ifdef __cplusplus
}
#endif

#endif
