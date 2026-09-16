/* SPDX-License-Identifier: MIT */
#include "w5_managed_runtime_guest.h"

#include "components/mem_service/providers/mem_service_provider_obmm.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

struct w5_managed_runtime_guest {
    struct mem_service_client client;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_obmm_endpoint provider;
    struct mem_service_access_domain *domain;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    char connect_spec[128];
};

int w5_managed_runtime_open(
    const struct w5_managed_runtime_config *config,
    struct w5_managed_runtime_guest **runtime_out)
{
    struct w5_managed_runtime_guest *runtime;
    struct mem_service_provider_obmm_config provider_config;
    struct mem_service_provider_registration registration;
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    size_t connect_length;
    int rc;
    if (!config || !runtime_out || !config->connect_spec ||
        !config->connect_spec[0] || !config->timeout_ms ||
        !config->import_region_bytes || !config->max_mappings ||
        config->max_mappings > MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS)
        return -EINVAL;
    *runtime_out = NULL;
    connect_length = strlen(config->connect_spec);
    if (connect_length >= sizeof(runtime->connect_spec)) return -ENAMETOOLONG;
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return -ENOMEM;
    *runtime_out = runtime;
    memcpy(runtime->connect_spec, config->connect_spec, connect_length + 1);
    runtime->client.connect_spec = runtime->connect_spec;
    mem_service_wire_client_options_init(&runtime->client.wire_options);
    runtime->client.wire_options.timeout_ms = config->timeout_ms;
    provider_config = (struct mem_service_provider_obmm_config) {
        .instance = "w5-managed-serving",
        .import_region_bytes = config->import_region_bytes,
        .max_remote_mappings = config->max_mappings,
    };
    rc = mem_service_provider_registry_init(&runtime->registry);
    if (!rc)
        rc = mem_service_provider_obmm_endpoint_open(&runtime->provider,
                                                     &provider_config);
    if (!rc)
        rc = mem_service_provider_obmm_endpoint_registration(&runtime->provider,
                                                             &registration);
    if (!rc)
        rc = mem_service_provider_registry_register(&runtime->registry,
                                                    &registration);
    if (!rc)
        rc = mem_service_provider_channel_bind(&runtime->registry,
            registration.name, registration.instance,
            MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING, &runtime->channel);
    if (!rc)
        rc = mem_service_access_domain_create(&runtime->client,
                                              &runtime->channel,
                                              &runtime->domain);
    if (!rc) rc = lingqu_shmem_mem_service_open_managed(&runtime->compute);
    if (!rc) rc = lingqu_shmem_pto_endpoint_open(&runtime->endpoint,
                                                 &endpoint_info);
    return rc;
}

const struct mem_service_client *w5_managed_runtime_client(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime ? &runtime->client : NULL;
}

const struct mem_service_provider_channel *w5_managed_runtime_channel(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime ? &runtime->channel : NULL;
}

struct mem_service_access_domain *w5_managed_runtime_domain(
    struct w5_managed_runtime_guest *runtime)
{
    return runtime ? runtime->domain : NULL;
}

struct lingqu_shmem_mem_service_context *w5_managed_runtime_compute(
    struct w5_managed_runtime_guest *runtime)
{
    return runtime ? runtime->compute : NULL;
}

struct lingqu_shmem_pto_endpoint *w5_managed_runtime_endpoint(
    struct w5_managed_runtime_guest *runtime)
{
    return runtime ? runtime->endpoint : NULL;
}

int w5_managed_runtime_close(struct w5_managed_runtime_guest **runtime_ptr)
{
    struct w5_managed_runtime_guest *runtime;
    int rc;
    if (!runtime_ptr || !*runtime_ptr) return -EINVAL;
    runtime = *runtime_ptr;
    if (runtime->endpoint) {
        rc = lingqu_shmem_pto_endpoint_close_checked(runtime->endpoint);
        if (rc) return rc;
        runtime->endpoint = NULL;
    }
    if (runtime->compute) {
        rc = lingqu_shmem_mem_service_close(runtime->compute);
        if (rc) return rc;
        runtime->compute = NULL;
    }
    if (runtime->domain) {
        rc = mem_service_access_domain_destroy(runtime->domain);
        if (rc) return rc;
        runtime->domain = NULL;
    }
    rc = mem_service_provider_obmm_endpoint_close_checked(&runtime->provider);
    if (rc) return rc;
    free(runtime);
    *runtime_ptr = NULL;
    return 0;
}
