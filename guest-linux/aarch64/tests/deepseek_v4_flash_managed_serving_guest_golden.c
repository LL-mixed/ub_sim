#include "deepseek_v4_flash_managed_serving_guest.h"

#include "components/mem_service/mem_service_object_contract.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
        exit(1); \
    } \
} while (0)

struct w5_managed_object_guest;

struct mem_service_mapping_owner {
    struct w5_managed_object_guest *object;
};

struct mem_service_mapping_lease {
    struct w5_managed_object_guest *object;
};

struct w5_managed_object_guest {
    struct mem_service_mapping_owner owner;
    struct mem_service_client_allocation allocation;
    struct lingqu_object_ref_wire_v2 reference;
    unsigned char *bytes;
    unsigned stages;
    int writer;
    int sealed;
};

struct lingqu_model_direct_operation {
    unsigned marker;
};

static struct lingqu_model_direct_memref captured[6];
static struct deepseek_v4_flash_managed_writer_layout expected_layout;
static struct w5_managed_object_guest *writer_object;
static unsigned acquires;
static unsigned publishes;
static unsigned waits;
static unsigned stages;
static unsigned releases;

uint64_t mem_service_provider_checksum64(const void *data, uint64_t len)
{
    const unsigned char *bytes = data;
    uint64_t hash = UINT64_C(1469598103934665603);

    for (uint64_t i = 0; i < len; ++i)
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    return hash;
}

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
    return object && !object->writer ? &object->reference : NULL;
}

int w5_managed_object_acquire_cpu(
    struct w5_managed_object_guest *object,
    uint64_t offset,
    uint64_t length,
    uint64_t flags,
    struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out)
{
    struct mem_service_mapping_lease *lease;

    CHECK(object && offset <= object->allocation.size_bytes);
    CHECK(length <= object->allocation.size_bytes - offset);
    CHECK(flags == (MEM_SERVICE_CLIENT_MAP_READ |
                    MEM_SERVICE_CLIENT_MAP_WRITE));
    lease = calloc(1, sizeof(*lease));
    CHECK(lease);
    lease->object = object;
    ++acquires;
    *lease_out = lease;
    *view_out = (struct mem_service_mapping_access_view) {
        .base = object->bytes + offset,
        .offset = offset,
        .len = length,
        .flags = flags,
        .generation = 1,
    };
    return 0;
}

int mem_service_mapping_lease_release(struct mem_service_mapping_lease *lease)
{
    CHECK(lease);
    free(lease);
    ++releases;
    return 0;
}

int w5_managed_object_publish_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum)
{
    CHECK(object == writer_object && lease && length);
    CHECK(checksum == mem_service_provider_checksum64(
                          object->bytes + offset, length));
    ++publishes;
    return 0;
}

int w5_managed_object_wait_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum)
{
    CHECK(object == writer_object && lease && length);
    CHECK(checksum == mem_service_provider_checksum64(
                          object->bytes + offset, length));
    ++waits;
    return 0;
}

int w5_managed_writer_stage(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *lease,
    const char *reference_key,
    const struct mem_service_client_reference_view *view,
    struct lingqu_object_ref_wire_v2 *reference_out,
    enum mem_service_wire_status *status_out)
{
    CHECK(object == writer_object && lease && reference_key && view);
    CHECK(view->offset + view->len <= object->allocation.size_bytes);
    memset(reference_out, 0, sizeof(*reference_out));
    reference_out->object.magic = LINGQU_OBJECT_REF_MAGIC;
    reference_out->object.layout_version =
        LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    reference_out->object.object_kind = view->object_kind;
    reference_out->object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    reference_out->object.object_version = 1;
    reference_out->object.payload_offset = view->offset;
    reference_out->object.payload_bytes = view->len;
    reference_out->object.payload_checksum =
        mem_service_provider_checksum64(object->bytes + view->offset,
                                        view->len);
    reference_out->wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    reference_out->access = LINGQU_OBJECT_REF_V2_READ;
    reference_out->allocation_generation = 1;
    reference_out->provider_incarnation = 1;
    reference_out->allocation_bytes = object->allocation.size_bytes;
    snprintf(reference_out->allocation_key,
             sizeof(reference_out->allocation_key), "%s",
             object->allocation.key);
    snprintf(reference_out->home_node, sizeof(reference_out->home_node),
             "guest-node-2");
    reference_out->object.key_hash = lingqu_object_ref_key_hash(
        reference_out->allocation_key,
        strlen(reference_out->allocation_key));
    ++object->stages;
    ++stages;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int w5_managed_writer_seal(
    struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out)
{
    CHECK(object == writer_object && object->stages == 4);
    object->sealed = 1;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int lingqu_model_direct_prepare(
    const struct lingqu_model_direct_request *request,
    struct lingqu_model_direct_operation **operation_out)
{
    CHECK(request && operation_out && request->callable_id == 5);
    CHECK(request->memref_count == 6 && request->scalar_count == 23);
    memcpy(captured, request->memrefs, sizeof(captured));
    CHECK(captured[0].owner != &writer_object->owner);
    CHECK(captured[1].owner != &writer_object->owner);
    for (unsigned i = 2; i < 6; ++i)
        CHECK(captured[i].owner == &writer_object->owner);
    CHECK(captured[0].spec.byte_offset == 0);
    CHECK(captured[1].spec.byte_offset == 0);
    CHECK(captured[2].spec.byte_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_TOKEN_IDS]
              .offset);
    CHECK(captured[3].spec.byte_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_HIDDEN_OUTPUT]
              .offset);
    CHECK(captured[4].spec.byte_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_NEXT_STATE]
              .offset);
    CHECK(captured[5].spec.byte_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_LOGITS]
              .offset);
    *operation_out = calloc(1, sizeof(**operation_out));
    CHECK(*operation_out);
    (*operation_out)->marker = 0xd54;
    return 0;
}

int lingqu_model_direct_submit(struct lingqu_model_direct_operation *operation)
{
    CHECK(operation && operation->marker == 0xd54);
    return 0;
}

int lingqu_model_direct_poll(
    struct lingqu_model_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion)
{
    CHECK(operation && operation->marker == 0xd54 && completion);
    memset(writer_object->bytes + captured[3].spec.byte_offset, 0x5a,
           captured[3].spec.byte_length);
    memset(writer_object->bytes + captured[4].spec.byte_offset, 0x6b,
           captured[4].spec.byte_length);
    for (uint32_t i = 0; i < captured[5].spec.shape[0]; ++i) {
        float value = (float)i;
        memcpy(writer_object->bytes + captured[5].spec.byte_offset +
                   i * sizeof(value),
               &value, sizeof(value));
    }
    *completion = (struct lingqu_shmem_pto_completion) {
        .op_id = 101,
        .status = 1,
    };
    return 0;
}

int lingqu_model_direct_release(
    struct lingqu_model_direct_operation **operation)
{
    CHECK(operation && *operation && (*operation)->marker == 0xd54);
    free(*operation);
    *operation = NULL;
    return 0;
}

static void init_object(struct w5_managed_object_guest *object,
                        const char *key, uint64_t bytes, int writer,
                        uint32_t kind)
{
    memset(object, 0, sizeof(*object));
    object->owner.object = object;
    object->bytes = calloc(1, bytes);
    CHECK(object->bytes);
    object->writer = writer;
    object->allocation.size_bytes = bytes;
    snprintf(object->allocation.key, sizeof(object->allocation.key), "%s",
             key);
    if (writer)
        return;
    object->reference.object.magic = LINGQU_OBJECT_REF_MAGIC;
    object->reference.object.layout_version =
        LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    object->reference.object.object_kind = kind;
    object->reference.object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    object->reference.object.object_version = 1;
    object->reference.object.payload_bytes = bytes;
    object->reference.object.payload_checksum =
        mem_service_provider_checksum64(object->bytes, bytes);
    object->reference.wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    object->reference.access = LINGQU_OBJECT_REF_V2_READ;
    object->reference.allocation_generation = 1;
    object->reference.provider_incarnation = 1;
    object->reference.allocation_bytes = bytes;
    snprintf(object->reference.allocation_key,
             sizeof(object->reference.allocation_key), "%s", key);
    snprintf(object->reference.home_node,
             sizeof(object->reference.home_node), "guest-node-1");
    object->reference.object.key_hash = lingqu_object_ref_key_hash(
        object->reference.allocation_key,
        strlen(object->reference.allocation_key));
}

int main(void)
{
    struct deepseek_v4_flash_managed_serving_operation *operation = NULL;
    struct deepseek_v4_flash_managed_serving_output output;
    struct deepseek_v4_flash_managed_serving_result result;
    struct lingqu_shmem_pto_completion completion;
    struct w5_managed_object_guest writer, hidden, state;
    struct deepseek_v4_flash_managed_direct_description description;
    struct deepseek_v4_flash_managed_direct_geometry geometry;
    struct deepseek_v4_flash_managed_serving_request request;
    enum mem_service_wire_status status;
    uint32_t token = 11;
    uint64_t token_words[8] = {7, 11, 10, 1000, 44, 55, 66, 77};

    CHECK(!deepseek_v4_flash_managed_direct_geometry_init(
        42, 43, 3, 1, &geometry));
    CHECK(!deepseek_v4_flash_managed_direct_describe(
        &geometry, &description));
    CHECK(!deepseek_v4_flash_managed_serving_layout(
        &geometry, 4096, &expected_layout));
    for (unsigned i = 1;
         i < DEEPSEEK_V4_FLASH_MANAGED_WRITER_VIEW_COUNT; ++i) {
        const struct deepseek_v4_flash_managed_writer_view *previous =
            &expected_layout.views[i - 1];
        const struct deepseek_v4_flash_managed_writer_view *current =
            &expected_layout.views[i];

        CHECK(current->offset % 64 == 0);
        CHECK(previous->offset + previous->bytes <= current->offset);
    }
    init_object(&writer, "writer", expected_layout.bytes + 4096, 1, 0);
    init_object(&hidden, "hidden",
        description
            .tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_HIDDEN_INPUT]
            .byte_length,
        0, MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT);
    init_object(&state, "state",
        description
            .tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_PREVIOUS_KV]
            .byte_length,
        0, MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE);
    writer_object = &writer;
    request = (struct deepseek_v4_flash_managed_serving_request) {
        .writer = &writer,
        .hidden_input = &hidden,
        .previous_state = &state,
        .geometry = geometry,
        .token_ids = &token,
        .token_count = 1,
        .writer_base_offset = 4096,
        .compute = (void *)1,
        .endpoint = (void *)2,
        .op_id = 101,
        .request_id = 202,
        .artifact_fingerprint = 303,
        .requester_cna = 7,
        .producer_node = 2,
        .decode_step = 7,
        .metadata = (void *)3,
        .metadata_bytes = 4096,
        .metadata_iova = 0x1000,
    };
    CHECK(!deepseek_v4_flash_managed_serving_prepare(&request, &operation));
    CHECK(!memcmp(writer.bytes + expected_layout
                      .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_TOKEN_IDS]
                      .offset,
                  &token, sizeof(token)));
    CHECK(!deepseek_v4_flash_managed_serving_submit(operation));
    CHECK(!deepseek_v4_flash_managed_serving_poll(operation, &completion));
    CHECK(completion.status == 1);
    CHECK(!deepseek_v4_flash_managed_serving_complete(operation, &output));
    CHECK(output.hidden && output.state && output.logits);
    CHECK(output.hidden_checksum && output.state_checksum &&
          output.logits_checksum);
    CHECK(!deepseek_v4_flash_managed_serving_publish(
        operation, token_words, &result, &status));
    CHECK(result.reference_count == 4 && writer.sealed);
    CHECK(result.references[0].object.payload_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_HIDDEN_OUTPUT]
              .offset);
    CHECK(result.references[1].object.payload_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_NEXT_STATE]
              .offset);
    CHECK(result.references[2].object.payload_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_LOGITS]
              .offset);
    CHECK(result.references[3].object.payload_offset ==
          expected_layout
              .views[DEEPSEEK_V4_FLASH_MANAGED_WRITER_TOKEN_RESULT]
              .offset);
    CHECK(!lingqu_object_ref_v2_validate(&result.references[3]));
    CHECK(result.references[3].object.object_kind ==
          MEM_SERVICE_OBMM_KIND_MODEL_TOKEN_RESULT);
    CHECK(!deepseek_v4_flash_managed_serving_release(&operation));
    CHECK(!operation && acquires == 2 && publishes == 2 && waits == 3 &&
          stages == 4 && releases == 2);
    free(writer.bytes);
    free(hidden.bytes);
    free(state.bytes);
    puts("deepseek_v4_flash_managed_serving_guest=pass callable=5 "
         "owners=direct views=nonoverlap publish=in_place outputs=4");
    return 0;
}
