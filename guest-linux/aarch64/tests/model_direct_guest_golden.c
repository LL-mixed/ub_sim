#include "lingqu_model_direct_guest.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
        exit(1); \
    } \
} while (0)

struct fake_lease {
    bool active;
    bool begun;
    bool completed;
};

static struct fake_lease leases[3];
static unsigned acquired, released, begun, completed, guards;
static int acquire_error_at = -1;
static int begin_error_at = -1;
static int complete_error_at = -1;
static int release_error_at = -1;
static int enqueue_error;
static int poll_error = -EAGAIN;
static uint64_t completion_id = 101;
static uint8_t completion_status = 1;

int lingqu_shmem_mem_service_acquire_owned(
    struct lingqu_shmem_mem_service_context *context,
    struct mem_service_mapping_owner *owner,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out,
    struct lingqu_shmem_mem_service_lease **lease_out)
{
    unsigned index = acquired++;
    CHECK(context && owner && index < 3);
    CHECK(spec->rank == 2 && spec->dtype == 1);
    CHECK(spec->shape[0] == 2 && spec->shape[1] == 4);
    CHECK(spec->strides[0] == 4 && spec->strides[1] == 1);
    CHECK(spec->byte_length == 16 && spec->byte_offset == index * 64);
    leases[index].active = true;
    *lease_out = (void *)&leases[index];
    if ((int)index == acquire_error_at) {
        *memref_out = NULL;
        return -EIO;
    }
    *memref_out = (void *)&leases[index];
    return 0;
}

int lingqu_shmem_mem_service_lease_begin(
    struct lingqu_shmem_mem_service_lease *lease, uint64_t op_id)
{
    struct fake_lease *item = (void *)lease;
    CHECK(op_id == 101 && item->active && !item->begun);
    if ((int)(item - leases) == begin_error_at) {
        begin_error_at = -1;
        return -EAGAIN;
    }
    item->begun = true;
    ++begun;
    return 0;
}

int lingqu_shmem_mem_service_lease_complete(
    struct lingqu_shmem_mem_service_lease *lease, uint64_t op_id)
{
    struct fake_lease *item = (void *)lease;
    CHECK(op_id == 101 && item->begun && !item->completed);
    if ((int)(item - leases) == complete_error_at) {
        complete_error_at = -1;
        return -EAGAIN;
    }
    item->completed = true;
    ++completed;
    return 0;
}

int lingqu_shmem_mem_service_release(
    struct lingqu_shmem_mem_service_lease *lease)
{
    struct fake_lease *item = (void *)lease;
    CHECK(item->active && (!item->begun || item->completed) && !guards);
    if ((int)(item - leases) == release_error_at) {
        release_error_at = -1;
        return -EIO;
    }
    item->active = false;
    ++released;
    return 0;
}

int lingqu_shmem_pto_dispatch_prepare(
    const struct lingqu_shmem_pto_request *request,
    void *metadata, size_t metadata_bytes, uint64_t metadata_iova,
    LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *wire,
    struct lingqu_shmem_pto_inflight **inflight_out)
{
    CHECK(request->op_id == 101 && request->request_id == 202);
    CHECK(request->callable_id == 41 && request->artifact_fingerprint == 303);
    CHECK(request->requester_cna == 7 && request->memref_count == 3);
    CHECK(request->scalar_count == 2 && metadata && metadata_bytes == 4096);
    CHECK(metadata_iova == 0x1000);
    CHECK(request->memrefs[0].arg_index == 0);
    CHECK(request->memrefs[1].arg_index == 2);
    CHECK(request->memrefs[2].arg_index == 4);
    CHECK(request->scalars[0].arg_index == 1);
    CHECK(request->scalars[1].arg_index == 3);
    memset(slot, 0, sizeof(*slot));
    memset(wire, 0, sizeof(*wire));
    *inflight_out = (void *)metadata;
    ++guards;
    return 0;
}

void lingqu_shmem_pto_dispatch_finish(struct lingqu_shmem_pto_inflight *inflight)
{
    CHECK(inflight && guards == 1);
    --guards;
}

int lingqu_shmem_pto_endpoint_enqueue(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot)
{
    CHECK(endpoint && slot && begun == acquired);
    return enqueue_error;
}

int lingqu_shmem_pto_endpoint_poll(
    struct lingqu_shmem_pto_endpoint *endpoint, uint64_t op_id,
    struct lingqu_shmem_pto_completion *completion)
{
    CHECK(endpoint && op_id == 101);
    if (poll_error) return poll_error;
    *completion = (struct lingqu_shmem_pto_completion) {
        .op_id = completion_id,
        .status = completion_status,
    };
    return 0;
}

static void reset(void)
{
    memset(leases, 0, sizeof(leases));
    acquired = released = begun = completed = guards = 0;
    acquire_error_at = begin_error_at = complete_error_at = -1;
    release_error_at = -1;
    enqueue_error = 0;
    poll_error = -EAGAIN;
    completion_id = 101;
    completion_status = 1;
}

static struct lingqu_model_direct_request request(
    struct lingqu_model_direct_memref memrefs[3],
    struct lingqu_shmem_pto_scalar_desc scalars[2],
    unsigned char metadata[4096])
{
    static int owners[3];
    for (uint32_t i = 0; i < 3; ++i) {
        memrefs[i] = (struct lingqu_model_direct_memref) {
            .owner = (void *)&owners[i],
            .spec = {
                .byte_offset = i * 64,
                .byte_length = 16,
                .rank = 2,
                .dtype = 1,
                .access = i == 2 ? LINGQU_SHMEM_ACCESS_WRITE :
                                   LINGQU_SHMEM_ACCESS_READ,
                .shape = {2, 4},
                .strides = {4, 1},
            },
            .arg_index = i * 2,
        };
    }
    scalars[0] = (struct lingqu_shmem_pto_scalar_desc) {
        .arg_index = 1, .dtype = 8, .value = 11,
    };
    scalars[1] = (struct lingqu_shmem_pto_scalar_desc) {
        .arg_index = 3, .dtype = 8, .value = 22,
    };
    return (struct lingqu_model_direct_request) {
        .compute = (void *)1,
        .endpoint = (void *)2,
        .op_id = 101,
        .request_id = 202,
        .callable_id = 41,
        .artifact_fingerprint = 303,
        .requester_cna = 7,
        .memrefs = memrefs,
        .memref_count = 3,
        .scalars = scalars,
        .scalar_count = 2,
        .metadata = metadata,
        .metadata_bytes = 4096,
        .metadata_iova = 0x1000,
    };
}

int main(void)
{
    struct lingqu_model_direct_memref memrefs[3];
    struct lingqu_shmem_pto_scalar_desc scalars[2];
    struct lingqu_model_direct_operation *operation = NULL;
    struct lingqu_shmem_pto_completion completion;
    unsigned char metadata[4096] = {0};
    struct lingqu_model_direct_request value;

    reset();
    value = request(memrefs, scalars, metadata);
    CHECK(!lingqu_model_direct_prepare(&value, &operation));
    CHECK(operation && acquired == 3);
    CHECK(!lingqu_model_direct_submit(operation));
    CHECK(lingqu_model_direct_poll(operation, &completion) == -EAGAIN);
    CHECK(lingqu_model_direct_release(&operation) == -EBUSY);
    poll_error = 0;
    CHECK(!lingqu_model_direct_poll(operation, &completion));
    CHECK(completion.status == 1 && completed == 3);
    CHECK(!lingqu_model_direct_release(&operation));
    CHECK(!operation && released == 3 && !guards);

    reset();
    value = request(memrefs, scalars, metadata);
    memrefs[1].arg_index = 0;
    CHECK(lingqu_model_direct_prepare(&value, &operation) == -EINVAL);
    CHECK(!operation && !acquired);

    reset();
    value = request(memrefs, scalars, metadata);
    acquire_error_at = 1;
    CHECK(lingqu_model_direct_prepare(&value, &operation) == -EIO);
    CHECK(operation && acquired == 2);
    CHECK(!lingqu_model_direct_release(&operation));
    CHECK(!operation && released == 2);

    reset();
    value = request(memrefs, scalars, metadata);
    CHECK(!lingqu_model_direct_prepare(&value, &operation));
    begin_error_at = 1;
    CHECK(lingqu_model_direct_submit(operation) == -EAGAIN);
    CHECK(!lingqu_model_direct_submit(operation));
    poll_error = 0;
    complete_error_at = 1;
    CHECK(lingqu_model_direct_poll(operation, &completion) == -EAGAIN);
    CHECK(!lingqu_model_direct_poll(operation, &completion));
    release_error_at = 1;
    CHECK(lingqu_model_direct_release(&operation) == -EIO);
    CHECK(operation);
    CHECK(!lingqu_model_direct_release(&operation));

    reset();
    value = request(memrefs, scalars, metadata);
    CHECK(!lingqu_model_direct_prepare(&value, &operation));
    enqueue_error = -EAGAIN;
    CHECK(lingqu_model_direct_submit(operation) == -EAGAIN);
    enqueue_error = 0;
    CHECK(!lingqu_model_direct_submit(operation));
    poll_error = 0;
    completion_status = 3;
    CHECK(!lingqu_model_direct_poll(operation, &completion));
    CHECK(completion.status == 3);
    CHECK(!lingqu_model_direct_release(&operation));

    puts("model_direct_guest=pass cases=5 staging_bytes=0");
    return 0;
}
