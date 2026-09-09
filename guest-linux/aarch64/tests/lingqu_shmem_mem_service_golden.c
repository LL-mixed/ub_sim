/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "components/mem_service/lingqu_object_service.h"
#include "components/mem_service/mem_service.h"
#include "lingqu_shmem_mem_service_backend.h"
#include "lingqu_shmem_pto.h"

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "check failed line=%d: %s\n", __LINE__,      \
                    #condition);                                            \
            return 1;                                                       \
        }                                                                   \
    } while (0)

struct fake_backend {
    uint8_t payload[4096];
    uint64_t next_offset;
    uint32_t acquire_count;
    uint32_t acquire_local_count;
    uint32_t acquire_local_kv_count;
    int kv_error;
    uint32_t release_count;
};

static int fake_acquire(
    void *backend_context,
    const struct mem_service_object_payload_view *view,
    struct lingqu_shmem_mem_service_region_binding *binding_out)
{
    struct fake_backend *backend = backend_context;

    if (!backend || !view || !binding_out ||
        view->data != backend->payload + view->backing_offset) {
        return -EINVAL;
    }
    backend->acquire_count++;
    *binding_out = (struct lingqu_shmem_mem_service_region_binding) {
        .region = {
            .mapped_addr = backend->payload,
            .mapped_length = sizeof(backend->payload),
            .ub_gm_addr = UINT64_C(0x200000),
            .opaque_mapping_ref = UINT64_C(0x703),
        },
        .object_byte_offset = view->backing_offset,
        .backend_lease = backend,
    };
    return 0;
}

static int fake_release(void *backend_context, void *backend_lease)
{
    struct fake_backend *backend = backend_context;

    if (!backend || backend_lease != backend) {
        return -EINVAL;
    }
    backend->release_count++;
    return 0;
}

static int fake_acquire_local(
    void *backend_context,
    uint64_t bytes,
    uint64_t align,
    struct lingqu_shmem_mem_service_region_binding *binding_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out)
{
    struct fake_backend *backend = backend_context;
    uint64_t offset;

    if (!backend || !binding_out || !buffer_out || bytes == 0 || align == 0) {
        return -EINVAL;
    }
    offset = (backend->next_offset + align - 1) & ~(align - 1);
    if (offset > sizeof(backend->payload) ||
        bytes > sizeof(backend->payload) - offset) {
        return -ENOSPC;
    }
    backend->next_offset = offset + bytes;
    backend->acquire_local_count++;
    *binding_out = (struct lingqu_shmem_mem_service_region_binding) {
        .region = {
            .mapped_addr = backend->payload,
            .mapped_length = sizeof(backend->payload),
            .ub_gm_addr = UINT64_C(0x200000),
            .opaque_mapping_ref = UINT64_C(0x704),
        },
        .object_byte_offset = offset,
        .backend_lease = backend,
    };
    *buffer_out = (struct lingqu_shmem_mem_service_local_buffer) {
        .data = backend->payload + offset,
        .len = bytes,
        .backing_offset = offset,
        .owner_node = 2,
    };
    return 0;
}

static int fake_acquire_local_kv(
    void *backend_context, uint64_t bytes, uint64_t align,
    struct lingqu_shmem_mem_service_region_binding *binding_out,
    struct lingqu_shmem_mem_service_local_buffer *buffer_out)
{
    struct fake_backend *backend = backend_context;
    (void)align;
    backend->acquire_local_kv_count++;
    if (backend->kv_error) return backend->kv_error;
    return fake_acquire_local(backend_context, bytes, 256, binding_out, buffer_out);
}

static const struct lingqu_shmem_mem_service_backend_ops fake_ops = {
    .acquire = fake_acquire,
    .acquire_local = fake_acquire_local,
    .acquire_local_kv = fake_acquire_local_kv,
    .release = fake_release,
};

int main(void)
{
    struct fake_backend backend = { 0 };
    struct mem_service_object_payload_view view = {
        .len = 64,
        .checksum = UINT64_C(0x1122334455667788),
        .owner_node = 1,
        .payload_kind = 6,
        .backing_offset = 128,
        .object_ref = {
            .magic = LINGQU_OBJECT_REF_MAGIC,
            .layout_version = LINGQU_OBJECT_REF_LAYOUT_VERSION,
            .object_kind = 6,
            .state = LINGQU_OBJECT_STATE_COMMITTED_WIRE,
            .owner_entity = 1,
            .producer_entity = 0,
            .object_version = 9,
            .key_hash = UINT64_C(0x99887766),
            .payload_offset = 128,
            .payload_bytes = 64,
            .payload_checksum = UINT64_C(0x1122334455667788),
        },
    };
    struct lingqu_shmem_memref_spec spec = {
        .byte_length = 64,
        .rank = 1,
        .dtype = 0,
        .access = LINGQU_SHMEM_ACCESS_READ,
        .shape = { 16 },
        .strides = { 1 },
    };
    struct lingqu_shmem_mem_service_context *context = NULL;
    struct lingqu_shmem_mem_service_lease *lease = NULL;
    struct lingqu_shmem_mem_service_local_buffer local_buffer;
    struct lingqu_shmem_memref *memref = NULL;
    struct lingqu_shmem_pto_memref_arg arg = { 0 };
    struct lingqu_shmem_pto_request request = {
        .op_id = 1,
        .request_id = 2,
        .callable_id = 3,
        .artifact_fingerprint = 4,
        .requester_cna = 5,
        .memrefs = &arg,
        .memref_count = 1,
    };
    struct lingqu_shmem_pto_inflight *inflight = NULL;
    struct lingqu_shmem_pto_wire_result wire_result;
    LingquPtoDispatchSlotV2 slot;
    LingquShmemMemrefV1 *wire_memref;
    uint8_t metadata[512] = { 0 };

    view.data = backend.payload + view.backing_offset;
    CHECK(lingqu_shmem_mem_service_context_create_for_backend(
              &fake_ops, &backend, false, &context) == 0);
    CHECK(lingqu_shmem_mem_service_acquire(
              context, &view, &spec, &memref, &lease) == 0);
    CHECK(memref != NULL);
    CHECK(lease != NULL);
    CHECK(backend.acquire_count == 1);
    CHECK(lingqu_shmem_mem_service_active_leases(context) == 1);
    CHECK(lingqu_shmem_mem_service_close(context) == -EBUSY);

    arg.memref = memref;
    arg.arg_index = 0;
    CHECK(lingqu_shmem_pto_dispatch_prepare(
              &request, metadata, sizeof(metadata), UINT64_C(0x100000),
              &slot, &wire_result, &inflight) == 0);
    wire_memref = (LingquShmemMemrefV1 *)(
        metadata + sizeof(LingquPtoDispatchControlV2));
    CHECK(wire_memref->opaque_mapping_ref == UINT64_C(0x703));
    CHECK(wire_memref->ub_gm_addr == UINT64_C(0x200000));
    CHECK(wire_memref->byte_offset == 128);
    CHECK(wire_memref->byte_length == 64);
    CHECK(lingqu_shmem_mem_service_release(lease) == -EBUSY);
    CHECK(backend.release_count == 0);
    lingqu_shmem_pto_dispatch_finish(inflight);
    inflight = NULL;
    CHECK(lingqu_shmem_mem_service_release(lease) == 0);
    lease = NULL;
    memref = NULL;
    CHECK(backend.release_count == 1);
    CHECK(lingqu_shmem_mem_service_active_leases(context) == 0);

    memset(&local_buffer, 0, sizeof(local_buffer));
    CHECK(lingqu_shmem_mem_service_acquire_local(
              context, &spec, &memref, &local_buffer, &lease) == 0);
    CHECK(backend.acquire_local_count == 1);
    CHECK(local_buffer.data == backend.payload + local_buffer.backing_offset);
    CHECK(local_buffer.len == spec.byte_length);
    CHECK(local_buffer.owner_node == 2);
    CHECK(lingqu_shmem_mem_service_active_leases(context) == 1);
    CHECK(lingqu_shmem_mem_service_release(lease) == 0);
    lease = NULL;
    memref = NULL;
    CHECK(backend.release_count == 2);
    CHECK(lingqu_shmem_mem_service_active_leases(context) == 0);

    view.object_ref.state = LINGQU_OBJECT_STATE_PENDING_WIRE;
    CHECK(lingqu_shmem_mem_service_acquire(
              context, &view, &spec, &memref, &lease) == -EINVAL);
    CHECK(backend.acquire_count == 1);
    view.object_ref.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    spec.byte_offset = 32;
    CHECK(lingqu_shmem_mem_service_acquire(
              context, &view, &spec, &memref, &lease) == -EINVAL);
    CHECK(backend.acquire_count == 1);
    CHECK(lingqu_shmem_mem_service_close(context) == 0);

    struct lingqu_shmem_mem_service_backend_ops no_kv_ops = fake_ops;
    no_kv_ops.acquire_local_kv = NULL;
    spec.byte_offset = 0;
    CHECK(lingqu_shmem_mem_service_context_create_for_backend(
              &no_kv_ops, &backend, false, &context) == 0);
    CHECK(lingqu_shmem_mem_service_acquire_local_kv(
              context, &spec, &memref, &local_buffer, &lease) == -EOPNOTSUPP);
    CHECK(!memref && !lease && !local_buffer.data);
    CHECK(backend.acquire_local_count == 1);
    CHECK(lingqu_shmem_mem_service_close(context) == 0);

    puts("lingqu_shmem_mem_service_golden=pass");
    return 0;
}
