/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_MODEL_DIRECT_GUEST_H
#define LINGQU_MODEL_DIRECT_GUEST_H

#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "components/mem_service/mem_service_mapping_owner.h"

#define LINGQU_MODEL_DIRECT_MAX_MEMREFS 32u
#define LINGQU_MODEL_DIRECT_MAX_SCALARS 32u

struct lingqu_model_direct_memref {
    struct mem_service_mapping_owner *owner;
    struct lingqu_shmem_memref_spec spec;
    uint32_t arg_index;
};

struct lingqu_model_direct_request {
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    uint64_t op_id;
    uint64_t request_id;
    uint64_t callable_id;
    uint64_t artifact_fingerprint;
    uint32_t requester_cna;
    const struct lingqu_model_direct_memref *memrefs;
    uint32_t memref_count;
    const struct lingqu_shmem_pto_scalar_desc *scalars;
    uint32_t scalar_count;
    void *metadata;
    size_t metadata_bytes;
    uint64_t metadata_iova;
};

struct lingqu_model_direct_operation;

/* Bind managed object views directly as PTO UB_GM tensors. Model payloads are
 * absent from this API, so prepare cannot materialize shared tensors in host
 * memory. Failed prepare may return a cleanup-only operation. */
int lingqu_model_direct_prepare(
    const struct lingqu_model_direct_request *request,
    struct lingqu_model_direct_operation **operation_out);

int lingqu_model_direct_submit(
    struct lingqu_model_direct_operation *operation);

/* -EAGAIN keeps the original operation and all mapping leases live. */
int lingqu_model_direct_poll(
    struct lingqu_model_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out);

/* Refuses release while the original dispatch can still access a tensor. */
int lingqu_model_direct_release(
    struct lingqu_model_direct_operation **operation);

#endif
