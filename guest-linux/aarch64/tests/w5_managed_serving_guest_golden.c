#include "w5_managed_serving_guest.h"
#include "components/mem_service/mem_service_object_contract.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem_service_mapping_owner { unsigned id; };
struct mem_service_mapping_lease { unsigned id; };
struct lingqu_shmem_mem_service_context { unsigned id; };
struct lingqu_shmem_pto_endpoint { unsigned id; };
struct w5_managed_model_operation { int pending; };
struct w5_managed_object_guest {
    struct mem_service_client_allocation allocation;
    struct lingqu_object_ref_wire_v2 reference;
    struct mem_service_mapping_owner owner;
    unsigned char *bytes;
    int reader;
    int sealed;
};

static unsigned char writer_bytes[65536];
static unsigned controls;
static unsigned publishes;
static unsigned waits;
static unsigned stages;
static unsigned releases;
static const char *stage_suffixes[] = {"/result", "/hidden", "/kv", "/terminal"};
static const unsigned stage_kinds[] = {
    MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
    MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT,
    MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE,
    MEM_SERVICE_OBMM_KIND_MODEL_TERMINAL_RESULT,
};

struct mem_service_mapping_owner *w5_managed_object_owner(
    struct w5_managed_object_guest *object)
{
    return object ? &object->owner : NULL;
}

const struct mem_service_client_allocation *w5_managed_object_allocation(
    const struct w5_managed_object_guest *object)
{
    return object ? &object->allocation : NULL;
}

const struct lingqu_object_ref_wire_v2 *w5_managed_object_reference(
    const struct w5_managed_object_guest *object)
{
    return object && object->reader ? &object->reference : NULL;
}

int w5_managed_object_acquire_cpu(
    struct w5_managed_object_guest *object, uint64_t offset, uint64_t length,
    uint64_t flags, struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out)
{
    static struct mem_service_mapping_lease lease = {1};
    assert(object && !object->reader && !offset && length == sizeof(writer_bytes));
    assert(flags == (MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE));
    *lease_out = &lease;
    *view_out = (struct mem_service_mapping_access_view) {
        .base = object->bytes, .len = length, .flags = flags, .generation = 9,
    };
    return 0;
}

int w5_managed_object_publish_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease, uint64_t offset,
    uint64_t length, uint64_t checksum)
{
    (void)object; (void)cpu_lease; (void)checksum;
    assert(offset == 0 && length == W5_MANAGED_MODEL_CONTROL_BYTES);
    ++publishes;
    return 0;
}

int w5_managed_object_wait_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease, uint64_t offset,
    uint64_t length, uint64_t checksum)
{
    (void)object; (void)cpu_lease;
    assert(offset == 16384 && length == 48000 && checksum == 0x9988);
    ++waits;
    return 0;
}

int w5_managed_writer_stage(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease, const char *reference_key,
    const struct mem_service_client_reference_view *view,
    struct lingqu_object_ref_wire_v2 *reference_out,
    enum mem_service_wire_status *status_out)
{
    (void)object; (void)cpu_lease;
    assert(stages < 4 && strstr(reference_key, stage_suffixes[stages]));
    assert(view->object_kind == stage_kinds[stages]);
    assert(view->owner_entity == 2 && view->producer_entity == 2);
    memset(reference_out, 0, sizeof(*reference_out));
    reference_out->object.object_kind = view->object_kind;
    reference_out->object.payload_offset = view->offset;
    reference_out->object.payload_bytes = view->len;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++stages;
    return 0;
}

int w5_managed_writer_seal(
    struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out)
{
    assert(stages == 4);
    object->sealed = 1;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_mapping_lease_release(struct mem_service_mapping_lease *lease)
{
    assert(lease && lease->id == 1);
    ++releases;
    return 0;
}

uint64_t mem_service_provider_checksum64(const void *data, uint64_t len)
{
    const unsigned char *bytes = data;
    uint64_t checksum = UINT64_C(1469598103934665603);
    for (uint64_t i = 0; i < len; ++i)
        checksum = (checksum ^ bytes[i]) * UINT64_C(1099511628211);
    return checksum;
}

int w5_managed_model_control_encode(void *buffer, size_t length,
    uint64_t op_id, uint64_t request_id, const uint32_t levels[8],
    const void *prefix, size_t prefix_length,
    const struct lingqu_object_ref_wire_v2 *references, unsigned input_count)
{
    assert(length == W5_MANAGED_MODEL_CONTROL_BYTES && op_id == 41 &&
           request_id == 42 && levels[0] == 0x5133060b &&
           prefix_length == 8 && !memcmp(prefix, "Q3PROMPT", 8) &&
           references && input_count == 2);
    memset(buffer, 0xa5, length);
    ++controls;
    return 0;
}

int w5_managed_model_prepare_views(
    struct lingqu_shmem_mem_service_context *context,
    struct mem_service_mapping_owner *const *owners, const uint64_t *offsets,
    const uint64_t *lengths, unsigned input_count, uint64_t op_id,
    uint64_t request_id, uint64_t fingerprint, uint32_t requester_cna,
    void *metadata, size_t metadata_bytes, uint64_t metadata_iova,
    struct w5_managed_model_operation **operation_out)
{
    (void)context; (void)metadata;
    assert(input_count == 2 && op_id == 41 && request_id == 42 &&
           fingerprint == 43 && requester_cna == 44 &&
           metadata_bytes == 4096 && metadata_iova == 0x1000);
    assert(owners[0]->id == 10 && owners[1]->id == 11 &&
           owners[2]->id == 12 && owners[3]->id == 10);
    assert(offsets[0] == 0 && offsets[1] == 0 && offsets[2] == 0 &&
           offsets[3] == 16384);
    assert(lengths[0] == W5_MANAGED_MODEL_CONTROL_BYTES &&
           lengths[1] == 2048 && lengths[2] == 8192 && lengths[3] == 49152);
    *operation_out = calloc(1, sizeof(**operation_out));
    return *operation_out ? 0 : -ENOMEM;
}

int w5_managed_model_submit(struct w5_managed_model_operation *operation,
    struct lingqu_shmem_pto_endpoint *endpoint)
{
    assert(operation && endpoint->id == 1 && !operation->pending);
    operation->pending = 1;
    return 0;
}

int w5_managed_model_poll(struct w5_managed_model_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    assert(operation && operation->pending);
    operation->pending = 0;
    *completion_out = (struct lingqu_shmem_pto_completion) {
        .op_id = 41, .status = 1,
    };
    return 0;
}

int w5_managed_model_release(struct w5_managed_model_operation *operation)
{
    if (operation->pending) return -EBUSY;
    free(operation);
    return 0;
}

bool lingqu_shmem_pto_completion_succeeded(
    const struct lingqu_shmem_pto_completion *completion)
{
    return completion && completion->status == 1;
}

int llm_infer_decode_range_result(const void *result, size_t result_bytes,
    uint64_t allocation_offset, const struct llm_infer_range_identity *expected,
    struct llm_infer_range_result *out)
{
    assert(result == writer_bytes + 16384 && result_bytes == 49152 &&
           allocation_offset == 16384 && expected->node == 2);
    memset(out, 0, sizeof(*out));
    out->serialized_bytes = 48000;
    out->serialized_checksum = 0x9988;
    out->hidden = (struct llm_infer_result_view) {
        .offset = 56000, .bytes = 2048, .service_checksum = 0x11,
    };
    out->kv = (struct llm_infer_result_view) {
        .offset = 58048, .bytes = 7488, .service_checksum = 0x22,
    };
    out->terminal_view = (struct llm_infer_result_view) {
        .offset = 20000, .bytes = 512, .service_checksum = 0x33,
    };
    out->terminal_present = 1;
    out->terminal.step = expected->step;
    out->terminal.sampled_token = 99;
    return 0;
}

static void fill_reader(struct w5_managed_object_guest *object, unsigned id,
                        unsigned kind, uint64_t bytes)
{
    memset(object, 0, sizeof(*object));
    object->reader = 1;
    object->owner.id = id;
    object->reference.object.magic = LINGQU_OBJECT_REF_MAGIC;
    object->reference.object.layout_version = LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    object->reference.object.object_kind = kind;
    object->reference.object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    object->reference.object.object_version = 3;
    object->reference.object.payload_bytes = bytes;
    object->reference.object.payload_checksum = id;
    object->reference.wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    object->reference.access = LINGQU_OBJECT_REF_V2_READ;
    object->reference.allocation_generation = 7;
    object->reference.provider_incarnation = 8;
    object->reference.allocation_bytes = bytes;
    snprintf(object->reference.allocation_key,
             sizeof(object->reference.allocation_key), "input-%u", id);
    strcpy(object->reference.home_node, "guest-node-1");
    object->reference.object.key_hash = lingqu_object_ref_key_hash(
        object->reference.allocation_key,
        strlen(object->reference.allocation_key));
    assert(lingqu_object_ref_v2_validate(&object->reference) == 0);
}

int main(void)
{
    struct w5_managed_object_guest writer = {0}, hidden = {0}, kv = {0};
    struct lingqu_shmem_mem_service_context compute = {1};
    struct lingqu_shmem_pto_endpoint endpoint = {1};
    unsigned char metadata[4096] = {0};
    struct w5_managed_serving_operation *operation = NULL;
    struct w5_managed_serving_result result;
    struct lingqu_shmem_pto_completion completion;
    enum mem_service_wire_status status;
    writer.bytes = writer_bytes;
    writer.owner.id = 10;
    strcpy(writer.allocation.key,
           "w5v1/0000000000000001/0000000000000002/00000003/0000000000000004");
    writer.allocation.size_bytes = sizeof(writer_bytes);
    fill_reader(&hidden, 11, MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT, 2048);
    fill_reader(&kv, 12, MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE, 8192);
    struct w5_managed_serving_request request = {
        .writer = &writer, .inputs = {&hidden, &kv}, .input_count = 2,
        .compute = &compute, .endpoint = &endpoint, .op_id = 41,
        .request_id = 42, .artifact_fingerprint = 43, .requester_cna = 44,
        .levels = {0x5133060b, 2, 3, 4, 3, 4, 28, 2048},
        .prompt_prefix = "Q3PROMPT", .prompt_prefix_bytes = 8,
        .output_offset = 16384, .output_capacity = 49152,
        .metadata = metadata, .metadata_bytes = sizeof(metadata),
        .metadata_iova = 0x1000,
        .expected = {.node = 2, .layer_start = 3, .layer_end = 4,
            .next_node = 3, .pipeline_nodes = 4, .total_layers = 28,
            .hidden_bytes = 2048, .step = 7, .vocab_size = 151936},
    };
    assert(w5_managed_serving_prepare(&request, &operation) == 0 && operation);
    assert(controls == 1 && publishes == 1);
    assert(w5_managed_serving_submit(operation) == 0);
    assert(w5_managed_serving_release(&operation) == -EBUSY && operation);
    assert(w5_managed_serving_poll(operation, &completion) == 0);
    assert(completion.op_id == 41 && completion.status == 1);
    assert(w5_managed_serving_finish(operation, &result, &status) == 0);
    assert(result.reference_count == 4 && result.decoded.terminal.sampled_token == 99);
    assert(writer.sealed && waits == 1 && stages == 4 && releases == 1);
    assert(w5_managed_serving_release(&operation) == 0 && !operation);
    puts("w5_managed_serving_guest=pass callable=4 inputs=2 outputs=4 terminal_v2=1");
    return 0;
}
