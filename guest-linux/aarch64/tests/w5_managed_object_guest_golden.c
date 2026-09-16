#include "w5_managed_object_guest.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem_service_access_domain {
    const struct mem_service_provider_channel *channel;
};
struct mem_service_mapping_lease;
struct mem_service_mapping_owner {
    const struct mem_service_provider_channel *channel;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle lifecycle;
    unsigned pins;
    int unmapped;
};
struct mem_service_mapping_lease {
    struct mem_service_mapping_owner *owner;
    struct mem_service_mapping_access_view view;
};

static unsigned allocations;
static unsigned begins;
static unsigned stages;
static unsigned seals;
static unsigned resolves;
static unsigned acquires;
static unsigned maps;
static unsigned adopts;
static unsigned releases;
static unsigned closes;
static unsigned destroys;
static unsigned char backing[65536];
static struct lingqu_object_ref_wire_v2 published;

static void fill_allocation(struct mem_service_client_allocation *allocation,
                            const char *session)
{
    memset(allocation, 0, sizeof(*allocation));
    strcpy(allocation->key, "w5v1/0000000000000001/0000000000000002/00000003/0000000000000004");
    strcpy(allocation->state, "active");
    strcpy(allocation->owner_session, session);
    allocation->generation = 7;
    allocation->version = begins ? 3 : 2;
    allocation->size_bytes = sizeof(backing);
    allocation->capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP;
    strcpy(allocation->home_node, "guest-node-3");
    allocation->provider_incarnation = 11;
    allocation->provider_backed = true;
    allocation->address = (uintptr_t)backing;
    allocation->address_len = sizeof(backing);
    allocation->holder_count = 1;
    strcpy(allocation->holders[0].session_id, session);
    allocation->holders[0].generation = allocation->generation;
}

static void fill_reference(struct lingqu_object_ref_wire_v2 *reference,
                           const struct mem_service_client_reference_view *view)
{
    memset(reference, 0, sizeof(*reference));
    reference->object.magic = LINGQU_OBJECT_REF_MAGIC;
    reference->object.layout_version = LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    reference->object.object_kind = view ? view->object_kind : 5;
    reference->object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    reference->object.owner_entity = view ? view->owner_entity : 3;
    reference->object.producer_entity = view ? view->producer_entity : 3;
    reference->object.object_version = 3;
    reference->object.payload_offset = view ? view->offset : 4096;
    reference->object.payload_bytes = view ? view->len : 2048;
    reference->object.payload_checksum = 0x1234;
    reference->wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    reference->access = LINGQU_OBJECT_REF_V2_READ;
    reference->allocation_generation = 7;
    reference->provider_incarnation = 11;
    reference->allocation_bytes = sizeof(backing);
    strcpy(reference->allocation_key,
           "w5v1/0000000000000001/0000000000000002/00000003/0000000000000004");
    strcpy(reference->home_node, "guest-node-3");
    reference->object.key_hash = lingqu_object_ref_key_hash(
        reference->allocation_key, strlen(reference->allocation_key));
    assert(lingqu_object_ref_v2_validate(reference) == 0);
}

int mem_service_client_allocate_object_at_home(
    const struct mem_service_client *client,
    const struct mem_service_client_allocate *request,
    const char *home_node,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    (void)client;
    assert(request->size_bytes == sizeof(backing));
    assert(request->capabilities == MEM_SERVICE_CLIENT_MANAGED_CAP_MAP);
    assert(strcmp(home_node, "guest-node-3") == 0);
    ++allocations;
    fill_allocation(allocation_out, request->session_id);
    strcpy(allocation_out->state, "allocating");
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_inspect_allocation(
    const struct mem_service_client *client, const char *key,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    (void)client;
    assert(key[0]);
    fill_allocation(allocation_out, "writer-session");
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_reference_transition(
    const struct mem_service_client *client,
    const struct mem_service_reference_request *request,
    struct mem_service_client_reference_result *result_out,
    enum mem_service_wire_status *status_out)
{
    (void)client;
    memset(result_out, 0, sizeof(*result_out));
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    switch (request->action) {
    case MEM_SERVICE_REFERENCE_BEGIN:
        assert(request->generation == 7 && request->version == 2);
        ++begins;
        fill_allocation(&result_out->allocation, request->session_id);
        break;
    case MEM_SERVICE_REFERENCE_STAGE:
        assert(lingqu_object_ref_v2_validate(&request->reference) == 0);
        ++stages;
        published = request->reference;
        fill_allocation(&result_out->allocation, request->session_id);
        break;
    case MEM_SERVICE_REFERENCE_SEAL:
        assert(request->generation == 7 && request->version == 3);
        ++seals;
        fill_allocation(&result_out->allocation, request->session_id);
        break;
    case MEM_SERVICE_REFERENCE_RESOLVE:
        assert(strstr(request->key, "/hidden") != NULL);
        ++resolves;
        fill_allocation(&result_out->allocation, "reader-session");
        result_out->reference = published;
        break;
    case MEM_SERVICE_REFERENCE_ACQUIRE:
        assert(request->access == LINGQU_OBJECT_REF_V2_READ);
        assert(memcmp(&request->reference, &published, sizeof(published)) == 0);
        ++acquires;
        fill_allocation(&result_out->allocation, request->session_id);
        result_out->reference = request->reference;
        break;
    default:
        assert(0);
    }
    return 0;
}

static void map_common(const struct mem_service_client_allocation *allocation,
                       uint64_t flags,
                       struct mem_service_client_object_mapping *mapping,
                       struct mem_service_client_mapping_lifecycle *lifecycle,
                       const char *session, const char *operation)
{
    memset(mapping, 0, sizeof(*mapping));
    strcpy(mapping->key, allocation->key);
    mapping->generation = allocation->generation;
    mapping->base = backing;
    mapping->len = sizeof(backing);
    mapping->flags = flags;
    mapping->binding.mapped = true;
    mapping->binding.mapping.handle = ++maps;
    mapping->binding.mapping.base = backing;
    mapping->binding.mapping.len = sizeof(backing);
    memset(lifecycle, 0, sizeof(*lifecycle));
    strcpy(lifecycle->operation_id, operation);
    strcpy(lifecycle->session_id, session);
    lifecycle->transaction.mapping_id = maps;
    lifecycle->transaction.generation = allocation->generation;
    lifecycle->transaction.state = 2;
    lifecycle->pending = true;
}

int mem_service_client_map_managed_allocation(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_allocation *allocation,
    const char *session_id, const char *operation_id, uint64_t flags,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    enum mem_service_wire_status *status_out)
{
    (void)client; (void)channel;
    map_common(allocation, flags, mapping, lifecycle, session_id, operation_id);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_map_managed_reference(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    const struct lingqu_object_ref_wire_v2 *reference,
    const char *session_id, const char *operation_id,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_reference_lifecycle *lifecycle,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_allocation allocation;
    (void)client; (void)channel;
    fill_allocation(&allocation, session_id);
    map_common(&allocation, MEM_SERVICE_CLIENT_MAP_READ, mapping,
               &lifecycle->mapping, session_id, operation_id);
    lifecycle->reference = *reference;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_mapping_owner_adopt(
    struct mem_service_access_domain *domain,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    const char *release_operation_id, uint32_t pin_limit,
    struct mem_service_mapping_owner **owner_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_mapping_owner *owner;
    assert(domain && release_operation_id[0] && pin_limit == 8);
    owner = calloc(1, sizeof(*owner));
    assert(owner);
    owner->channel = domain->channel;
    owner->mapping = *mapping;
    owner->lifecycle = *lifecycle;
    memset(mapping, 0, sizeof(*mapping));
    memset(lifecycle, 0, sizeof(*lifecycle));
    *owner_out = owner;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++adopts;
    return 0;
}

int mem_service_mapping_owner_acquire(
    struct mem_service_mapping_owner *owner,
    enum mem_service_mapping_access_kind kind,
    uint64_t offset, uint64_t len, uint64_t flags,
    struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out)
{
    struct mem_service_mapping_lease *lease;
    assert(kind == MEM_SERVICE_MAPPING_ACCESS_CPU);
    assert(offset <= owner->mapping.len && len <= owner->mapping.len - offset);
    assert((flags & ~owner->mapping.flags) == 0);
    lease = calloc(1, sizeof(*lease));
    assert(lease);
    lease->owner = owner;
    lease->view = (struct mem_service_mapping_access_view) {
        .base = (unsigned char *)owner->mapping.base + offset,
        .offset = offset, .len = len, .flags = flags,
        .generation = owner->mapping.generation,
    };
    ++owner->pins;
    *lease_out = lease;
    *view_out = lease->view;
    return 0;
}

int mem_service_mapping_lease_apply(
    struct mem_service_mapping_lease *lease,
    mem_service_mapping_access_fn callback, void *context)
{
    return callback(context, lease->owner->channel, &lease->owner->mapping,
                    &lease->owner->lifecycle, &lease->view);
}

int mem_service_provider_channel_publish_range(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t offset, uint64_t len, uint64_t expected_checksum,
    struct mem_service_visibility_completion *completion_out)
{
    assert(channel && binding->mapped && offset == 0 && len == 64);
    *completion_out = (struct mem_service_visibility_completion) {
        .visible_bytes = len, .checksum = expected_checksum,
    };
    return 0;
}

int mem_service_provider_channel_wait_range_visible(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_mapping_binding *binding,
    uint64_t offset, uint64_t len, uint64_t expected_checksum,
    uint64_t timeout_ms,
    struct mem_service_visibility_completion *completion_out)
{
    assert(timeout_ms == 1000);
    return mem_service_provider_channel_publish_range(channel, binding, offset,
        len, expected_checksum, completion_out);
}

int mem_service_mapping_lease_release(struct mem_service_mapping_lease *lease)
{
    assert(lease && lease->owner->pins);
    --lease->owner->pins;
    free(lease);
    return 0;
}

int mem_service_client_prepare_managed_reference(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_allocation *allocation,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_client_reference_view *view,
    struct lingqu_object_ref_wire_v2 *reference_out,
    enum mem_service_wire_status *status_out)
{
    (void)client; (void)channel; (void)allocation;
    assert(mapping->binding.mapped && lifecycle->pending);
    fill_reference(reference_out, view);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_mapping_owner_unmap(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out)
{
    if (owner->pins) return -EBUSY;
    owner->unmapped = 1;
    owner->mapping.binding.mapped = false;
    owner->lifecycle.pending = false;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_mapping_owner_close(
    struct mem_service_mapping_owner *owner,
    enum mem_service_wire_status *status_out)
{
    if (owner->pins) return -EBUSY;
    owner->unmapped = 1;
    ++closes;
    ++releases;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_mapping_owner_destroy(struct mem_service_mapping_owner *owner)
{
    assert(owner->unmapped && !owner->pins);
    ++destroys;
    free(owner);
    return 0;
}

int mem_service_client_unmap_managed_allocation(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_mapping_lifecycle *lifecycle,
    enum mem_service_wire_status *status_out)
{
    (void)client; (void)channel; (void)mapping; (void)lifecycle; (void)status_out;
    assert(0);
}

int mem_service_client_unmap_managed_reference(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_reference_lifecycle *lifecycle,
    enum mem_service_wire_status *status_out)
{
    (void)client; (void)channel; (void)mapping; (void)lifecycle; (void)status_out;
    assert(0);
}

int mem_service_client_release_object(
    const struct mem_service_client *client, const char *key,
    const char *idempotency_key, const char *session_id,
    bool has_expected_generation, uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    (void)client; (void)key; (void)idempotency_key; (void)session_id;
    (void)has_expected_generation; (void)expected_generation;
    (void)allocation_out; (void)status_out;
    assert(0);
}

int main(void)
{
    struct mem_service_client client = {.connect_spec = "unix:/tmp/service"};
    struct mem_service_provider_channel channel = {0};
    struct mem_service_access_domain domain = {&channel};
    struct w5_managed_object_guest *writer = NULL;
    struct w5_managed_object_guest *reader = NULL;
    struct mem_service_mapping_lease *cpu = NULL;
    struct mem_service_mapping_access_view access;
    struct lingqu_object_ref_wire_v2 reference;
    enum mem_service_wire_status status;
    const char *key =
        "w5v1/0000000000000001/0000000000000002/00000003/0000000000000004";
    char reference_key[96];
    struct w5_managed_writer_config writer_config = {
        .key = key, .home_node = "guest-node-3", .session_id = "writer-session",
        .operation_prefix = "writer-op", .size_bytes = sizeof(backing),
        .timeout_ms = 1000, .pin_limit = 8,
    };
    assert(w5_managed_writer_open(&client, &channel, &domain, &writer_config,
                                  &writer, &status) == 0);
    assert(writer && w5_managed_object_owner(writer));
    assert(w5_managed_object_allocation(writer)->version == 3);
    assert(w5_managed_object_close(&writer, &status) == -EBUSY);
    assert(w5_managed_object_acquire_cpu(writer, 0, sizeof(backing),
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &cpu, &access) == 0);
    memset(access.base, 0x5a, access.len);
    assert(w5_managed_object_publish_range(writer, cpu, 0, 64, 0x55) == 0);
    assert(w5_managed_object_wait_range(writer, cpu, 0, 64, 0x55) == 0);
    struct mem_service_client_reference_view view = {
        .offset = 4096, .len = 2048, .object_kind = 5,
        .owner_entity = 3, .producer_entity = 3,
    };
    snprintf(reference_key, sizeof(reference_key), "%s/hidden", key);
    assert(w5_managed_writer_stage(writer, cpu, reference_key, &view,
                                   &reference, &status) == 0);
    assert(reference.object.payload_offset == view.offset);
    assert(mem_service_mapping_lease_release(cpu) == 0);
    assert(w5_managed_writer_seal(writer, &status) == 0);
    assert(w5_managed_object_close(&writer, &status) == 0 && !writer);

    struct w5_managed_reader_config reader_config = {
        .reference_key = reference_key, .session_id = "reader-session",
        .operation_prefix = "reader-op", .timeout_ms = 1000, .pin_limit = 8,
    };
    assert(w5_managed_reader_open(&client, &channel, &domain, &reader_config,
                                  &reader, &status) == 0);
    assert(reader && w5_managed_object_reference(reader));
    assert(w5_managed_object_reference(reader)->object.payload_bytes == 2048);
    assert(w5_managed_object_acquire_cpu(reader, 4096, 2048,
        MEM_SERVICE_CLIENT_MAP_READ, &cpu, &access) == 0);
    assert(access.base == backing + 4096 && access.len == 2048);
    assert(mem_service_mapping_lease_release(cpu) == 0);
    assert(w5_managed_object_close(&reader, &status) == 0 && !reader);

    assert(allocations == 1 && begins == 1 && stages == 1 && seals == 1);
    assert(resolves == 1 && acquires == 1 && maps == 2 && adopts == 2);
    assert(closes == 2 && destroys == 2 && releases == 2);
    puts("w5_managed_object_guest=pass writer_lifecycle=1 reader_lifecycle=1 owner_cleanup=1");
    return 0;
}
