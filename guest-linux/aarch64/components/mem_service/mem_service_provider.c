#include "mem_service_provider.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define MEM_SERVICE_PROVIDER_REGION_WIRE_MAGIC 0x4d535250U
#define MEM_SERVICE_PROVIDER_CAP_VALID_MASK \
    (MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION | \
     MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER | \
     MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER | \
     MEM_SERVICE_PROVIDER_CAP_DURABLE_STORAGE | \
     MEM_SERVICE_PROVIDER_CAP_ACCELERATOR_MEMORY)

struct mem_service_provider_fixture_context {
    enum mem_service_provider_state state;
    uint8_t *source;
    uint8_t *destination;
    uint64_t region_len;
    uint64_t completion_id;
    uint64_t pending_bytes;
    uint64_t pending_checksum;
};

static bool mem_service_provider_identity_valid(const char *value, size_t max_len)
{
    size_t i;
    size_t len;

    if (value == NULL) {
        return false;
    }
    len = strlen(value);
    if (len == 0 || len >= max_len) {
        return false;
    }
    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)value[i];

        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != ':') {
            return false;
        }
    }
    return true;
}

static bool mem_service_provider_ops_valid(
    const struct mem_service_provider_registration *registration)
{
    uint64_t capabilities;

    if (registration == NULL || registration->ops == NULL ||
        registration->ops->probe == NULL) {
        return false;
    }
    capabilities = registration->capabilities;
    if (capabilities == 0 ||
        (capabilities & ~MEM_SERVICE_PROVIDER_CAP_VALID_MASK) != 0) {
        return false;
    }
    if ((capabilities & MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION) != 0 &&
        (registration->ops->register_region == NULL ||
         registration->ops->deregister_region == NULL)) {
        return false;
    }
    if ((capabilities & MEM_SERVICE_PROVIDER_CAP_TRANSFER_MASK) != 0 &&
        (registration->ops->submit_transfer == NULL ||
         registration->ops->poll_completion == NULL)) {
        return false;
    }
    if ((capabilities & MEM_SERVICE_PROVIDER_CAP_ACCELERATOR_MEMORY) != 0 &&
        (capabilities & MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION) == 0) {
        return false;
    }
    return true;
}

const char *mem_service_provider_state_name(enum mem_service_provider_state state)
{
    switch (state) {
    case MEM_SERVICE_PROVIDER_STATE_READY:
        return "ready";
    case MEM_SERVICE_PROVIDER_STATE_DEGRADED:
        return "degraded";
    case MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE:
        return "unavailable";
    default:
        return "unknown";
    }
}

int mem_service_provider_registry_init(
    struct mem_service_provider_registry *registry)
{
    if (registry == NULL) {
        return -1;
    }
    memset(registry, 0, sizeof(*registry));
    registry->initialized = true;
    return 0;
}

const struct mem_service_provider *mem_service_provider_registry_find(
    const struct mem_service_provider_registry *registry,
    const char *name,
    const char *instance)
{
    size_t i;

    if (registry == NULL || !registry->initialized || name == NULL ||
        instance == NULL) {
        return NULL;
    }
    for (i = 0; i < registry->count; ++i) {
        const struct mem_service_provider *provider = &registry->providers[i];

        if (strcmp(provider->name, name) == 0 &&
            strcmp(provider->instance, instance) == 0) {
            return provider;
        }
    }
    return NULL;
}

int mem_service_provider_registry_register(
    struct mem_service_provider_registry *registry,
    const struct mem_service_provider_registration *registration)
{
    struct mem_service_provider *provider;
    enum mem_service_provider_state state = MEM_SERVICE_PROVIDER_STATE_UNKNOWN;

    if (registry == NULL || !registry->initialized || registration == NULL ||
        !mem_service_provider_identity_valid(registration->name,
                                             MEM_SERVICE_PROVIDER_NAME_LEN) ||
        !mem_service_provider_identity_valid(registration->instance,
                                             MEM_SERVICE_PROVIDER_INSTANCE_LEN) ||
        !mem_service_provider_ops_valid(registration) ||
        registry->count >= MEM_SERVICE_MAX_PROVIDERS ||
        mem_service_provider_registry_find(registry,
                                           registration->name,
                                           registration->instance) != NULL) {
        return -1;
    }
    if (registration->ops->probe(registration->context, &state) != 0 ||
        state < MEM_SERVICE_PROVIDER_STATE_UNKNOWN ||
        state > MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE) {
        return -1;
    }
    provider = &registry->providers[registry->count];
    memset(provider, 0, sizeof(*provider));
    snprintf(provider->name, sizeof(provider->name), "%s", registration->name);
    snprintf(provider->instance,
             sizeof(provider->instance),
             "%s",
             registration->instance);
    provider->capabilities = registration->capabilities;
    provider->state = state;
    provider->ops = registration->ops;
    provider->context = registration->context;
    registry->count += 1U;
    return 0;
}

int mem_service_provider_registry_refresh(
    struct mem_service_provider_registry *registry)
{
    size_t i;
    int failures = 0;

    if (registry == NULL || !registry->initialized) {
        return -1;
    }
    for (i = 0; i < registry->count; ++i) {
        struct mem_service_provider *provider = &registry->providers[i];
        enum mem_service_provider_state state =
            MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;

        if (provider->ops == NULL || provider->ops->probe == NULL ||
            provider->ops->probe(provider->context, &state) != 0 ||
            state < MEM_SERVICE_PROVIDER_STATE_UNKNOWN ||
            state > MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE) {
            provider->state = MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
            failures += 1;
            continue;
        }
        provider->state = state;
    }
    return failures == 0 ? 0 : -1;
}

size_t mem_service_provider_registry_ready_count(
    const struct mem_service_provider_registry *registry)
{
    size_t count = 0;
    size_t i;

    if (registry == NULL || !registry->initialized) {
        return 0;
    }
    for (i = 0; i < registry->count; ++i) {
        if (registry->providers[i].state == MEM_SERVICE_PROVIDER_STATE_READY) {
            count += 1U;
        }
    }
    return count;
}

bool mem_service_provider_registry_data_plane_ready(
    const struct mem_service_provider_registry *registry)
{
    bool transfer_provider_found = false;
    size_t i;

    if (registry == NULL || !registry->initialized) {
        return false;
    }
    for (i = 0; i < registry->count; ++i) {
        const struct mem_service_provider *provider = &registry->providers[i];

        if ((provider->capabilities &
             MEM_SERVICE_PROVIDER_CAP_TRANSFER_MASK) == 0) {
            continue;
        }
        transfer_provider_found = true;
        if (provider->state != MEM_SERVICE_PROVIDER_STATE_READY) {
            return false;
        }
    }
    return transfer_provider_found;
}

static bool mem_service_memory_kind_valid(enum mem_service_memory_kind kind)
{
    return kind == MEM_SERVICE_MEMORY_HOST ||
           kind == MEM_SERVICE_MEMORY_ACCELERATOR ||
           kind == MEM_SERVICE_MEMORY_PERSISTENT;
}

static void mem_service_provider_wire_put_u32(uint8_t *wire, uint32_t value)
{
    wire[0] = (uint8_t)(value >> 24);
    wire[1] = (uint8_t)(value >> 16);
    wire[2] = (uint8_t)(value >> 8);
    wire[3] = (uint8_t)value;
}

static uint32_t mem_service_provider_wire_get_u32(const uint8_t *wire)
{
    return ((uint32_t)wire[0] << 24) |
           ((uint32_t)wire[1] << 16) |
           ((uint32_t)wire[2] << 8) |
           (uint32_t)wire[3];
}

static void mem_service_provider_wire_put_u64(uint8_t *wire, uint64_t value)
{
    mem_service_provider_wire_put_u32(wire, (uint32_t)(value >> 32));
    mem_service_provider_wire_put_u32(wire + 4, (uint32_t)value);
}

static uint64_t mem_service_provider_wire_get_u64(const uint8_t *wire)
{
    return ((uint64_t)mem_service_provider_wire_get_u32(wire) << 32) |
           mem_service_provider_wire_get_u32(wire + 4);
}

static bool mem_service_provider_channel_ready(
    const struct mem_service_provider_channel *channel)
{
    enum mem_service_provider_state state =
        MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;

    return channel != NULL && channel->provider != NULL &&
           channel->provider->ops != NULL &&
           channel->provider->ops->probe != NULL &&
           channel->provider->ops->probe(channel->provider->context,
                                         &state) == 0 &&
           state == MEM_SERVICE_PROVIDER_STATE_READY;
}

uint64_t mem_service_provider_checksum64(const void *data, uint64_t len)
{
    const uint8_t *bytes = data;
    uint64_t checksum = 1469598103934665603ULL;
    uint64_t i;

    if (data == NULL && len != 0) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        checksum ^= bytes[i];
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

int mem_service_provider_channel_bind(
    const struct mem_service_provider_registry *registry,
    const char *name,
    const char *instance,
    uint64_t required_capabilities,
    struct mem_service_provider_channel *channel_out)
{
    const struct mem_service_provider *provider;
    struct mem_service_provider_channel channel;

    if (channel_out == NULL || required_capabilities == 0 ||
        (required_capabilities & ~MEM_SERVICE_PROVIDER_CAP_VALID_MASK) != 0 ||
        !mem_service_provider_registry_data_plane_ready(registry) ||
        (provider = mem_service_provider_registry_find(
             registry, name, instance)) == NULL ||
        (provider->capabilities & required_capabilities) !=
            required_capabilities) {
        return -1;
    }
    memset(&channel, 0, sizeof(channel));
    channel.provider = provider;
    channel.required_capabilities = required_capabilities;
    if (!mem_service_provider_channel_ready(&channel)) {
        return -1;
    }
    *channel_out = channel;
    return 0;
}

int mem_service_provider_channel_register_region(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_region_request *request,
    struct mem_service_provider_region_binding *binding_out)
{
    struct mem_service_provider_region_binding binding;

    if (!mem_service_provider_channel_ready(channel) || request == NULL ||
        binding_out == NULL || request->base == NULL || request->len == 0 ||
        !mem_service_memory_kind_valid(request->memory_kind) ||
        (channel->provider->capabilities &
         MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION) == 0 ||
        channel->provider->ops->register_region == NULL) {
        return -1;
    }
    memset(&binding, 0, sizeof(binding));
    if (channel->provider->ops->register_region(channel->provider->context,
                                                request,
                                                &binding.region) != 0 ||
        binding.region.handle == 0 ||
        binding.region.len != request->len ||
        binding.region.memory_kind != request->memory_kind ||
        binding.region.descriptor.len >
            MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN) {
        if (binding.region.handle != 0 &&
            channel->provider->ops->deregister_region != NULL) {
            (void)channel->provider->ops->deregister_region(
                channel->provider->context, binding.region.handle);
        }
        return -1;
    }
    binding.owner = channel->provider;
    binding.registered = true;
    *binding_out = binding;
    return 0;
}

int mem_service_provider_channel_export_region(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *binding,
    struct mem_service_provider_remote_region *remote_out)
{
    if (!mem_service_provider_channel_ready(channel) || binding == NULL ||
        !binding->registered || binding->owner != channel->provider ||
        remote_out == NULL ||
        binding->region.len == 0 ||
        !mem_service_memory_kind_valid(binding->region.memory_kind) ||
        binding->region.descriptor.len == 0 ||
        binding->region.descriptor.len >
            MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN) {
        return -1;
    }
    memset(remote_out, 0, sizeof(*remote_out));
    snprintf(remote_out->provider_name,
             sizeof(remote_out->provider_name),
             "%s",
             channel->provider->name);
    remote_out->len = binding->region.len;
    remote_out->memory_kind = binding->region.memory_kind;
    remote_out->descriptor = binding->region.descriptor;
    return 0;
}

int mem_service_provider_remote_region_encode(
    const struct mem_service_provider_remote_region *remote,
    void *wire_out,
    size_t wire_capacity,
    size_t *wire_len_out)
{
    uint8_t *wire = wire_out;
    size_t provider_len;
    size_t wire_len;

    if (remote == NULL || wire_out == NULL || wire_len_out == NULL ||
        !mem_service_provider_identity_valid(
            remote->provider_name, MEM_SERVICE_PROVIDER_NAME_LEN) ||
        remote->len == 0 ||
        !mem_service_memory_kind_valid(remote->memory_kind) ||
        remote->descriptor.len == 0 ||
        remote->descriptor.len > MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN) {
        return -1;
    }
    provider_len = strlen(remote->provider_name);
    wire_len = 32U + provider_len + remote->descriptor.len;
    if (wire_len > wire_capacity ||
        wire_len > MEM_SERVICE_PROVIDER_REGION_WIRE_MAX_LEN) {
        return -1;
    }
    memset(wire, 0, wire_len);
    mem_service_provider_wire_put_u32(
        wire, MEM_SERVICE_PROVIDER_REGION_WIRE_MAGIC);
    mem_service_provider_wire_put_u32(
        wire + 4, MEM_SERVICE_PROVIDER_REGION_WIRE_VERSION);
    mem_service_provider_wire_put_u32(wire + 8, (uint32_t)provider_len);
    mem_service_provider_wire_put_u32(
        wire + 12, remote->descriptor.len);
    mem_service_provider_wire_put_u32(
        wire + 16, (uint32_t)remote->memory_kind);
    mem_service_provider_wire_put_u64(wire + 24, remote->len);
    memcpy(wire + 32, remote->provider_name, provider_len);
    memcpy(wire + 32 + provider_len,
           remote->descriptor.bytes,
           remote->descriptor.len);
    *wire_len_out = wire_len;
    return 0;
}

int mem_service_provider_remote_region_decode(
    const void *wire_data,
    size_t wire_len,
    struct mem_service_provider_remote_region *remote_out)
{
    const uint8_t *wire = wire_data;
    struct mem_service_provider_remote_region remote;
    uint32_t provider_len;
    uint32_t descriptor_len;
    uint32_t memory_kind;
    uint64_t region_len;
    size_t expected_len;

    if (wire == NULL || remote_out == NULL || wire_len < 32U ||
        wire_len > MEM_SERVICE_PROVIDER_REGION_WIRE_MAX_LEN ||
        mem_service_provider_wire_get_u32(wire) !=
            MEM_SERVICE_PROVIDER_REGION_WIRE_MAGIC ||
        mem_service_provider_wire_get_u32(wire + 4) !=
            MEM_SERVICE_PROVIDER_REGION_WIRE_VERSION ||
        mem_service_provider_wire_get_u32(wire + 20) != 0) {
        return -1;
    }
    provider_len = mem_service_provider_wire_get_u32(wire + 8);
    descriptor_len = mem_service_provider_wire_get_u32(wire + 12);
    memory_kind = mem_service_provider_wire_get_u32(wire + 16);
    region_len = mem_service_provider_wire_get_u64(wire + 24);
    expected_len = 32U + (size_t)provider_len + descriptor_len;
    if (provider_len == 0 ||
        provider_len >= MEM_SERVICE_PROVIDER_NAME_LEN ||
        descriptor_len == 0 ||
        descriptor_len > MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN ||
        expected_len != wire_len || region_len == 0 ||
        !mem_service_memory_kind_valid(
            (enum mem_service_memory_kind)memory_kind)) {
        return -1;
    }
    memset(&remote, 0, sizeof(remote));
    memcpy(remote.provider_name, wire + 32, provider_len);
    remote.provider_name[provider_len] = '\0';
    if (!mem_service_provider_identity_valid(
            remote.provider_name, sizeof(remote.provider_name))) {
        return -1;
    }
    remote.len = region_len;
    remote.memory_kind = (enum mem_service_memory_kind)memory_kind;
    remote.descriptor.len = descriptor_len;
    memcpy(remote.descriptor.bytes,
           wire + 32 + provider_len,
           descriptor_len);
    *remote_out = remote;
    return 0;
}

int mem_service_provider_channel_transfer(
    const struct mem_service_provider_channel *channel,
    const struct mem_service_provider_region_binding *source,
    uint64_t source_offset,
    const struct mem_service_provider_remote_region *destination,
    uint64_t destination_offset,
    uint64_t len,
    uint64_t expected_checksum,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_transfer_request request;
    struct mem_service_transfer_completion completion;
    uint64_t completion_id = 0;

    if (!mem_service_provider_channel_ready(channel) || source == NULL ||
        !source->registered || source->owner != channel->provider ||
        destination == NULL ||
        completion_out == NULL || len == 0 || expected_checksum == 0 ||
        strcmp(destination->provider_name,
               channel->provider->name) != 0 ||
        destination->descriptor.len == 0 ||
        destination->descriptor.len >
            MEM_SERVICE_PROVIDER_DESCRIPTOR_LEN ||
        !mem_service_memory_kind_valid(destination->memory_kind) ||
        source_offset > source->region.len ||
        len > source->region.len - source_offset ||
        destination_offset > destination->len ||
        len > destination->len - destination_offset ||
        (channel->provider->capabilities &
         MEM_SERVICE_PROVIDER_CAP_TRANSFER_MASK) == 0 ||
        channel->provider->ops->submit_transfer == NULL ||
        channel->provider->ops->poll_completion == NULL) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.source.region_handle = source->region.handle;
    request.source.offset = source_offset;
    request.source.len = len;
    request.source.descriptor = source->region.descriptor;
    request.destination.offset = destination_offset;
    request.destination.len = len;
    request.destination.descriptor = destination->descriptor;
    request.expected_checksum = expected_checksum;
    if (channel->provider->ops->submit_transfer(
            channel->provider->context,
            &request,
            &completion_id) != 0 ||
        completion_id == 0 ||
        channel->provider->ops->poll_completion(
            channel->provider->context,
            completion_id,
            &completion) != 0 ||
        completion.id != completion_id || completion.status != 0 ||
        completion.transferred_bytes != len ||
        completion.checksum != expected_checksum) {
        return -1;
    }
    *completion_out = completion;
    return 0;
}

int mem_service_provider_channel_deregister_region(
    const struct mem_service_provider_channel *channel,
    struct mem_service_provider_region_binding *binding)
{
    if (channel == NULL || channel->provider == NULL || binding == NULL ||
        !binding->registered || binding->owner != channel->provider ||
        channel->provider->ops == NULL ||
        channel->provider->ops->deregister_region == NULL ||
        channel->provider->ops->deregister_region(
            channel->provider->context, binding->region.handle) != 0) {
        return -1;
    }
    memset(binding, 0, sizeof(*binding));
    return 0;
}

static int mem_service_provider_fixture_probe(
    void *context,
    enum mem_service_provider_state *state_out)
{
    struct mem_service_provider_fixture_context *fixture = context;

    if (fixture == NULL || state_out == NULL) {
        return -1;
    }
    *state_out = fixture->state;
    return 0;
}

static int mem_service_provider_fixture_register_region(
    void *context,
    const struct mem_service_region_request *request,
    struct mem_service_region *region_out)
{
    struct mem_service_provider_fixture_context *fixture = context;

    if (fixture == NULL || request == NULL || region_out == NULL ||
        request->base == NULL || request->len == 0 ||
        request->len > fixture->region_len) {
        return -1;
    }
    memset(region_out, 0, sizeof(*region_out));
    region_out->handle =
        request->base == fixture->source ? 1U : 2U;
    region_out->len = request->len;
    region_out->memory_kind = request->memory_kind;
    region_out->descriptor.len = sizeof(region_out->handle);
    memcpy(region_out->descriptor.bytes,
           &region_out->handle,
           sizeof(region_out->handle));
    return 0;
}

static int mem_service_provider_fixture_deregister_region(
    void *context,
    uint64_t region_handle)
{
    return context != NULL && (region_handle == 1U || region_handle == 2U)
               ? 0
               : -1;
}

static int mem_service_provider_fixture_submit_transfer(
    void *context,
    const struct mem_service_transfer_request *request,
    uint64_t *completion_id_out)
{
    struct mem_service_provider_fixture_context *fixture = context;
    uint64_t destination_handle = 0;

    if (fixture == NULL || request == NULL || completion_id_out == NULL ||
        request->source.region_handle != 1U ||
        request->source.len == 0 ||
        request->source.len != request->destination.len ||
        request->destination.descriptor.len !=
            sizeof(destination_handle) ||
        request->source.offset > fixture->region_len ||
        request->source.len > fixture->region_len - request->source.offset ||
        request->destination.offset > fixture->region_len ||
        request->destination.len >
            fixture->region_len - request->destination.offset) {
        return -1;
    }
    memcpy(&destination_handle,
           request->destination.descriptor.bytes,
           sizeof(destination_handle));
    if (destination_handle != 2U) {
        return -1;
    }
    memcpy(fixture->destination + request->destination.offset,
           fixture->source + request->source.offset,
           (size_t)request->source.len);
    fixture->completion_id += 1U;
    fixture->pending_bytes = request->source.len;
    fixture->pending_checksum = mem_service_provider_checksum64(
        fixture->source + request->source.offset,
        request->source.len);
    *completion_id_out = fixture->completion_id;
    return 0;
}

static int mem_service_provider_fixture_poll_completion(
    void *context,
    uint64_t completion_id,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_provider_fixture_context *fixture = context;

    if (fixture == NULL || completion_out == NULL ||
        completion_id == 0 || completion_id != fixture->completion_id) {
        return -1;
    }
    memset(completion_out, 0, sizeof(*completion_out));
    completion_out->id = completion_id;
    completion_out->status = 0;
    completion_out->transferred_bytes = fixture->pending_bytes;
    completion_out->checksum = fixture->pending_checksum;
    return 0;
}

int mem_service_run_provider_fixture_check(void)
{
    static const struct mem_service_provider_ops valid_ops = {
        .probe = mem_service_provider_fixture_probe,
        .register_region = mem_service_provider_fixture_register_region,
        .deregister_region = mem_service_provider_fixture_deregister_region,
        .submit_transfer = mem_service_provider_fixture_submit_transfer,
        .poll_completion = mem_service_provider_fixture_poll_completion,
    };
    static const struct mem_service_provider_ops invalid_ops = {
        .probe = mem_service_provider_fixture_probe,
    };
    struct mem_service_provider_registry registry;
    struct mem_service_provider_fixture_context fixture;
    struct mem_service_provider_fixture_context degraded_fixture;
    struct mem_service_provider_registration registration;
    struct mem_service_region_request region_request;
    struct mem_service_region source_region;
    struct mem_service_region destination_region;
    struct mem_service_transfer_request transfer;
    struct mem_service_transfer_completion completion;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_region_binding source_binding;
    struct mem_service_provider_region_binding destination_binding;
    struct mem_service_provider_remote_region remote_region;
    struct mem_service_provider_remote_region decoded_region;
    uint8_t source[16] = "provider-check";
    uint8_t destination[16] = {0};
    uint8_t descriptor_wire[MEM_SERVICE_PROVIDER_REGION_WIRE_MAX_LEN];
    size_t descriptor_wire_len = 0;
    uint64_t expected_checksum;
    uint64_t completion_id = 0;

    memset(&fixture, 0, sizeof(fixture));
    fixture.state = MEM_SERVICE_PROVIDER_STATE_READY;
    fixture.source = source;
    fixture.destination = destination;
    fixture.region_len = sizeof(source);
    degraded_fixture = fixture;
    degraded_fixture.state = MEM_SERVICE_PROVIDER_STATE_DEGRADED;
    if (mem_service_provider_registry_init(&registry) != 0 ||
        mem_service_provider_registry_data_plane_ready(&registry) ||
        mem_service_provider_registry_ready_count(&registry) != 0) {
        return 1;
    }
    registration = (struct mem_service_provider_registration){
        .name = "fixture",
        .instance = "local-0",
        .capabilities = MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                        MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER,
        .ops = &valid_ops,
        .context = &fixture,
    };
    if (mem_service_provider_registry_register(&registry, &registration) != 0 ||
        mem_service_provider_registry_register(&registry, &registration) == 0 ||
        mem_service_provider_registry_ready_count(&registry) != 1 ||
        !mem_service_provider_registry_data_plane_ready(&registry)) {
        return 1;
    }
    registration.instance = "invalid-ops";
    registration.context = &fixture;
    registration.ops = &invalid_ops;
    if (mem_service_provider_registry_register(&registry, &registration) == 0) {
        return 1;
    }
    registration.instance = "bad identity";
    registration.capabilities = MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION;
    registration.ops = &valid_ops;
    if (mem_service_provider_registry_register(&registry, &registration) == 0) {
        return 1;
    }
    region_request = (struct mem_service_region_request){
        .base = source,
        .len = sizeof(source),
        .memory_kind = MEM_SERVICE_MEMORY_HOST,
    };
    if (valid_ops.register_region(&fixture, &region_request, &source_region) != 0) {
        return 1;
    }
    region_request.base = destination;
    if (valid_ops.register_region(&fixture,
                                  &region_request,
                                  &destination_region) != 0) {
        return 1;
    }
    memset(&transfer, 0, sizeof(transfer));
    transfer.source.region_handle = source_region.handle;
    transfer.source.len = sizeof(source);
    transfer.source.descriptor = source_region.descriptor;
    transfer.destination.region_handle = destination_region.handle;
    transfer.destination.len = sizeof(destination);
    transfer.destination.descriptor = destination_region.descriptor;
    if (valid_ops.submit_transfer(&fixture, &transfer, &completion_id) != 0 ||
        valid_ops.poll_completion(&fixture,
                                  completion_id,
                                  &completion) != 0 ||
        completion.status != 0 ||
        completion.transferred_bytes != sizeof(source) ||
        memcmp(source, destination, sizeof(source)) != 0) {
        return 1;
    }
    transfer.source.offset = sizeof(source);
    transfer.source.len = 1;
    transfer.destination.len = 1;
    if (valid_ops.submit_transfer(&fixture, &transfer, &completion_id) == 0) {
        return 1;
    }
    memset(destination, 0, sizeof(destination));
    if (mem_service_provider_channel_bind(
            &registry,
            "fixture",
            "local-0",
            MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER,
            &channel) != 0) {
        return 1;
    }
    region_request.base = source;
    region_request.len = sizeof(source);
    region_request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    if (mem_service_provider_channel_register_region(
            &channel, &region_request, &source_binding) != 0) {
        return 1;
    }
    region_request.base = destination;
    if (mem_service_provider_channel_register_region(
            &channel, &region_request, &destination_binding) != 0 ||
        mem_service_provider_channel_export_region(
            &channel, &destination_binding, &remote_region) != 0 ||
        mem_service_provider_remote_region_encode(
            &remote_region,
            descriptor_wire,
            sizeof(descriptor_wire),
            &descriptor_wire_len) != 0 ||
        mem_service_provider_remote_region_decode(
            descriptor_wire,
            descriptor_wire_len,
            &decoded_region) != 0) {
        return 1;
    }
    expected_checksum =
        mem_service_provider_checksum64(source, sizeof(source));
    if (expected_checksum == 0 ||
        mem_service_provider_channel_transfer(
            &channel,
            &source_binding,
            0,
            &decoded_region,
            0,
            sizeof(source),
            expected_checksum,
            &completion) != 0 ||
        memcmp(source, destination, sizeof(source)) != 0) {
        return 1;
    }
    descriptor_wire[20] = 1U;
    if (mem_service_provider_remote_region_decode(
            descriptor_wire,
            descriptor_wire_len,
            &decoded_region) == 0) {
        return 1;
    }
    descriptor_wire[20] = 0U;
    if (mem_service_provider_remote_region_decode(
            descriptor_wire,
            descriptor_wire_len,
            &decoded_region) != 0) {
        return 1;
    }
    snprintf(decoded_region.provider_name,
             sizeof(decoded_region.provider_name),
             "%s",
             "other");
    if (mem_service_provider_channel_transfer(
            &channel,
            &source_binding,
            0,
            &decoded_region,
            0,
            sizeof(source),
            expected_checksum,
            &completion) == 0) {
        return 1;
    }
    if (mem_service_provider_channel_deregister_region(
            &channel, &source_binding) != 0 ||
        mem_service_provider_channel_deregister_region(
            &channel, &destination_binding) != 0) {
        return 1;
    }
    registration.instance = "degraded-peer";
    registration.context = &degraded_fixture;
    registration.capabilities =
        MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
        MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER;
    registration.ops = &valid_ops;
    if (mem_service_provider_registry_register(&registry, &registration) != 0 ||
        mem_service_provider_registry_ready_count(&registry) != 1 ||
        mem_service_provider_registry_data_plane_ready(&registry) ||
        mem_service_provider_channel_bind(
            &registry,
            "fixture",
            "local-0",
            MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER,
            &channel) == 0) {
        return 1;
    }
    fixture.state = MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
    if (mem_service_provider_registry_refresh(&registry) != 0 ||
        mem_service_provider_registry_data_plane_ready(&registry) ||
        mem_service_provider_registry_ready_count(&registry) != 0 ||
        mem_service_provider_channel_bind(
            &registry,
            "fixture",
            "local-0",
            MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                MEM_SERVICE_PROVIDER_CAP_LOCAL_TRANSFER,
            &channel) == 0) {
        return 1;
    }
    printf("mem_service provider-fixtures: status=ok providers=%zu "
           "ready=%zu data_plane_ready=%u all_required=fail-closed "
           "bounds=fail-closed sdk=ok descriptor_wire=fail-closed\n",
           registry.count,
           mem_service_provider_registry_ready_count(&registry),
           mem_service_provider_registry_data_plane_ready(&registry) ? 1U : 0U);
    return 0;
}
