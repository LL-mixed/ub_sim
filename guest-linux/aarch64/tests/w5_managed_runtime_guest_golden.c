#include "w5_managed_runtime_guest.h"
#include "components/mem_service/providers/mem_service_provider_obmm.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct mem_service_access_domain { unsigned id; };
struct lingqu_shmem_mem_service_context { unsigned id; };
struct lingqu_shmem_pto_endpoint { unsigned id; };

static struct mem_service_provider provider;
static unsigned closed;
static int endpoint_open_result;
static unsigned canary_prepared;
static unsigned canary_exchanged;
static unsigned canary_verified;

uint64_t mem_service_provider_checksum64(const void *data, uint64_t len)
{
    const uint8_t *bytes = data;
    uint64_t checksum = UINT64_C(1469598103934665603);

    assert(data || !len);
    for (uint64_t i = 0; i < len; ++i) {
        checksum ^= bytes[i];
        checksum *= UINT64_C(1099511628211);
    }
    return checksum;
}

int mem_service_provider_obmm_probe_device(const char *device_path,
                                           const char *primary_cna_path,
                                           char *detail,
                                           size_t detail_len)
{
    assert(!device_path && !primary_cna_path);
    assert(detail && detail_len >= 32);
    snprintf(detail, detail_len,
             "device=/dev/obmm primary_cna=0xc4c2 mapping_path=sim-dec");
    return 0;
}

void mem_service_wire_client_options_init(
    struct mem_service_wire_client_options *options)
{
    memset(options, 0, sizeof(*options));
}

int mem_service_provider_registry_init(
    struct mem_service_provider_registry *registry)
{
    memset(registry, 0, sizeof(*registry));
    registry->initialized = true;
    return 0;
}

int mem_service_provider_obmm_endpoint_open(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_obmm_config *config)
{
    assert(!strcmp(config->instance, "w5-managed-serving"));
    assert(config->import_region_bytes == 64 * 1024 * 1024);
    assert(config->max_remote_mappings == 8);
    assert(config->required_peer_mappings == 1);
    assert(config->force_osync);
    if (endpoint_open_result) return endpoint_open_result;
    endpoint->implementation = endpoint;
    return 0;
}

int mem_service_provider_obmm_endpoint_registration(
    struct mem_service_provider_obmm_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out)
{
    assert(endpoint->implementation);
    *registration_out = (struct mem_service_provider_registration) {
        .name = "obmm", .instance = "w5-managed-serving",
        .capabilities = MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING,
    };
    return 0;
}

int mem_service_provider_obmm_endpoint_prepare_canary_region(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint64_t region_len,
    uint64_t visible_len,
    uint8_t seed,
    struct mem_service_region *region_out,
    struct mem_service_provider_remote_region *remote_out,
    uint64_t *checksum_out)
{
    assert(endpoint->implementation);
    assert(region_len == 64 * 1024 * 1024);
    assert(visible_len == 256 && seed == 42);
    memset(region_out, 0, sizeof(*region_out));
    memset(remote_out, 0, sizeof(*remote_out));
    region_out->handle = 1;
    region_out->len = region_len;
    strcpy(remote_out->provider_name, "obmm");
    remote_out->len = region_len;
    remote_out->memory_kind = MEM_SERVICE_MEMORY_HOST;
    *checksum_out = 1;
    ++canary_prepared;
    return 0;
}

int mem_service_provider_obmm_endpoint_exchange_remote_regions(
    struct mem_service_provider_obmm_endpoint *endpoint,
    uint32_t local_node,
    uint32_t node_count,
    uint64_t generation,
    const struct mem_service_provider_remote_region *local,
    struct mem_service_provider_remote_region *regions_out,
    size_t region_capacity)
{
    assert(endpoint->implementation && local);
    assert(local_node == 1 && node_count == 2);
    assert(generation == 0x12345678 && region_capacity >= node_count);
    regions_out[0] = *local;
    regions_out[1] = *local;
    ++canary_exchanged;
    return 0;
}

int mem_service_provider_obmm_endpoint_verify_mapping(
    struct mem_service_provider_obmm_endpoint *endpoint,
    const struct mem_service_provider_remote_region *remote,
    uint64_t offset,
    uint64_t len,
    uint64_t expected_checksum,
    uint64_t timeout_ms)
{
    assert(endpoint->implementation && remote);
    assert(offset == 0 && len == 256 && expected_checksum != 0);
    assert(timeout_ms == 300000);
    ++canary_verified;
    return 0;
}

int mem_service_provider_registry_register(
    struct mem_service_provider_registry *registry,
    const struct mem_service_provider_registration *registration)
{
    assert(registry->initialized && registration->capabilities);
    registry->count = 1;
    strcpy(provider.name, registration->name);
    strcpy(provider.instance, registration->instance);
    provider.capabilities = registration->capabilities;
    provider.state = MEM_SERVICE_PROVIDER_STATE_READY;
    return 0;
}

int mem_service_provider_channel_bind(
    const struct mem_service_provider_registry *registry,
    const char *name, const char *instance, uint64_t required_capabilities,
    struct mem_service_provider_channel *channel_out)
{
    assert(registry->count == 1 && !strcmp(name, "obmm") &&
           !strcmp(instance, "w5-managed-serving") &&
           required_capabilities == MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING);
    channel_out->provider = &provider;
    channel_out->required_capabilities = required_capabilities;
    return 0;
}

int mem_service_access_domain_create(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain **domain_out)
{
    assert(!strcmp(client->connect_spec, "unix:/tmp/mem-service.sock"));
    assert(client->wire_options.timeout_ms == 300000 && channel->provider);
    *domain_out = calloc(1, sizeof(**domain_out));
    (*domain_out)->id = 1;
    return 0;
}

int lingqu_shmem_mem_service_open_managed(
    struct lingqu_shmem_mem_service_context **context_out)
{
    *context_out = calloc(1, sizeof(**context_out));
    (*context_out)->id = 2;
    return 0;
}

int lingqu_shmem_pto_endpoint_open(
    struct lingqu_shmem_pto_endpoint **endpoint_out,
    struct lingqu_shmem_pto_endpoint_info *info_out)
{
    (void)info_out;
    *endpoint_out = calloc(1, sizeof(**endpoint_out));
    (*endpoint_out)->id = 3;
    return 0;
}

int lingqu_shmem_pto_endpoint_close_checked(
    struct lingqu_shmem_pto_endpoint *endpoint)
{
    assert(endpoint->id == 3);
    free(endpoint);
    ++closed;
    return 0;
}

int lingqu_shmem_mem_service_close(
    struct lingqu_shmem_mem_service_context *context)
{
    assert(context->id == 2);
    free(context);
    ++closed;
    return 0;
}

int mem_service_access_domain_destroy(struct mem_service_access_domain *domain)
{
    assert(domain->id == 1);
    free(domain);
    ++closed;
    return 0;
}

int mem_service_provider_obmm_endpoint_close_checked(
    struct mem_service_provider_obmm_endpoint *endpoint)
{
    if (!endpoint->implementation) return 0;
    endpoint->implementation = NULL;
    ++closed;
    return 0;
}

int main(void)
{
    struct w5_managed_runtime_guest *runtime = NULL;
    struct w5_managed_runtime_config invalid_config;
    struct w5_managed_runtime_config config = {
        .connect_spec = "unix:/tmp/mem-service.sock",
        .client_node_id = "guest-node-3",
        .client_provider_incarnation = 1000003,
        .timeout_ms = 300000,
        .import_region_bytes = 64 * 1024 * 1024,
        .allocation_granularity_bytes = 64 * 1024 * 1024,
        .bootstrap_generation = 0x12345678,
        .max_mappings = 8,
        .local_node = 1,
        .node_count = 2,
    };
    invalid_config = config;
    invalid_config.allocation_granularity_bytes = 0;
    assert(w5_managed_runtime_open(&invalid_config, &runtime) == -EINVAL);
    invalid_config.allocation_granularity_bytes = 3 * 1024 * 1024;
    assert(w5_managed_runtime_open(&invalid_config, &runtime) == -EINVAL);
    invalid_config.allocation_granularity_bytes = 128 * 1024 * 1024;
    assert(w5_managed_runtime_open(&invalid_config, &runtime) == -EINVAL);
    assert(w5_managed_runtime_open(&config, &runtime) == 0 && runtime);
    assert(!strcmp(w5_managed_runtime_open_stage(runtime), "ready"));
    assert(strstr(w5_managed_runtime_open_detail(runtime),
                  "device=/dev/obmm"));
    assert(w5_managed_runtime_client(runtime));
    assert(w5_managed_runtime_channel(runtime)->provider == &provider);
    assert(w5_managed_runtime_domain(runtime));
    assert(w5_managed_runtime_compute(runtime));
    assert(w5_managed_runtime_endpoint(runtime));
    assert(!strcmp(w5_managed_runtime_client_node_id(runtime),
                   "guest-node-3"));
    assert(w5_managed_runtime_client_provider_incarnation(runtime) ==
           1000003);
    assert(w5_managed_runtime_close(&runtime) == 0 && !runtime);
    assert(closed == 4);
    assert(canary_prepared == 1);
    assert(canary_exchanged == 1);
    assert(canary_verified == 1);
    endpoint_open_result = -EPERM;
    assert(w5_managed_runtime_open(&config, &runtime) == -EPERM && runtime);
    assert(!strcmp(w5_managed_runtime_open_stage(runtime), "obmm-endpoint"));
    assert(strstr(w5_managed_runtime_open_detail(runtime),
                  "primary_cna=0xc4c2"));
    assert(w5_managed_runtime_close(&runtime) == 0 && !runtime);
    assert(closed == 4);
    puts("w5_managed_runtime_guest=pass provider_selected_by_adapter=1 cleanup=4");
    return 0;
}
