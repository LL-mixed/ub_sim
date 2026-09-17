/* SPDX-License-Identifier: MIT */
#include "w5_managed_runtime_guest.h"

#include "components/mem_service/providers/mem_service_provider_obmm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W5_MANAGED_CANARY_REGION_BYTES (2U * 1024U * 1024U)
#define W5_MANAGED_CANARY_VISIBLE_BYTES 256U
#define W5_MANAGED_CANARY_SEED_BASE 41U

struct w5_managed_runtime_guest {
    struct mem_service_client client;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_obmm_endpoint provider;
    struct mem_service_access_domain *domain;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_pto_endpoint *endpoint;
    char connect_spec[128];
    char client_node_id[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    char open_detail[256];
    const char *open_stage;
    uint64_t client_provider_incarnation;
};

int w5_managed_runtime_open(
    const struct w5_managed_runtime_config *config,
    struct w5_managed_runtime_guest **runtime_out)
{
    struct w5_managed_runtime_guest *runtime;
    struct mem_service_provider_obmm_config provider_config;
    struct mem_service_provider_registration registration;
    struct mem_service_provider_remote_region local_canary;
    struct mem_service_provider_remote_region
        peer_canaries[MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS];
    struct mem_service_region canary_region;
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    uint8_t canary_pattern[W5_MANAGED_CANARY_VISIBLE_BYTES];
    uint64_t canary_checksum;
    char probe_detail[256] = "";
    size_t connect_length, node_length;
    uint32_t peer;
    int rc;
    if (!config || !runtime_out || !config->connect_spec ||
        !config->connect_spec[0] || !config->timeout_ms ||
        !config->client_node_id || !config->client_node_id[0] ||
        !config->client_provider_incarnation ||
        !config->import_region_bytes || !config->bootstrap_generation ||
        !config->max_mappings ||
        config->max_mappings > MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS ||
        config->node_count < 2 ||
        config->node_count > MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS ||
        config->local_node >= config->node_count ||
        config->max_mappings < config->node_count - 1U)
        return -EINVAL;
    *runtime_out = NULL;
    connect_length = strlen(config->connect_spec);
    node_length = strlen(config->client_node_id);
    if (connect_length >= sizeof(runtime->connect_spec) ||
        node_length >= sizeof(runtime->client_node_id)) return -ENAMETOOLONG;
    runtime = calloc(1, sizeof(*runtime));
    if (!runtime) return -ENOMEM;
    *runtime_out = runtime;
    runtime->open_stage = "allocated";
    memcpy(runtime->connect_spec, config->connect_spec, connect_length + 1);
    memcpy(runtime->client_node_id, config->client_node_id, node_length + 1);
    runtime->client_provider_incarnation =
        config->client_provider_incarnation;
    runtime->client.connect_spec = runtime->connect_spec;
    mem_service_wire_client_options_init(&runtime->client.wire_options);
    runtime->client.wire_options.timeout_ms = config->timeout_ms;
    provider_config = (struct mem_service_provider_obmm_config) {
        .instance = "w5-managed-serving",
        .import_region_bytes = config->import_region_bytes,
        .max_remote_mappings = config->max_mappings,
        .required_peer_mappings = config->node_count - 1U,
    };
    rc = mem_service_provider_obmm_probe_device(
        provider_config.device_path, provider_config.primary_cna_path,
        probe_detail, sizeof(probe_detail));
    runtime->open_stage = "obmm-probe";
    snprintf(runtime->open_detail, sizeof(runtime->open_detail), "%s",
             probe_detail);
    if (rc != 0) {
        fprintf(stderr,
                "[w5_managed_runtime] open stage=obmm-probe rc=%d %s\n",
                rc, probe_detail);
        return rc;
    }
    runtime->open_stage = "registry-init";
    rc = mem_service_provider_registry_init(&runtime->registry);
    if (rc != 0)
        fprintf(stderr,
                "[w5_managed_runtime] open stage=registry-init rc=%d\n", rc);
    if (!rc) {
        runtime->open_stage = "obmm-endpoint";
        rc = mem_service_provider_obmm_endpoint_open(&runtime->provider,
                                                     &provider_config);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=obmm-endpoint rc=%d"
                    " import_region=%llu max_mappings=%u required_peers=%u %s\n",
                    rc, (unsigned long long)provider_config.import_region_bytes,
                    provider_config.max_remote_mappings,
                    provider_config.required_peer_mappings, probe_detail);
    }
    if (!rc) {
        runtime->open_stage = "canary-prepare";
        rc = mem_service_provider_obmm_endpoint_prepare_canary_region(
            &runtime->provider, W5_MANAGED_CANARY_REGION_BYTES,
            W5_MANAGED_CANARY_VISIBLE_BYTES,
            (uint8_t)(W5_MANAGED_CANARY_SEED_BASE + config->local_node),
            &canary_region, &local_canary, &canary_checksum);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=canary-prepare rc=%d\n",
                    rc);
    }
    if (!rc) {
        runtime->open_stage = "canary-exchange";
        rc = mem_service_provider_obmm_endpoint_exchange_remote_regions(
            &runtime->provider, config->local_node, config->node_count,
            config->bootstrap_generation, &local_canary, peer_canaries,
            MEM_SERVICE_PROVIDER_OBMM_MAX_MAPPINGS);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=canary-exchange rc=%d"
                    " generation=%llu node=%u/%u\n",
                    rc, (unsigned long long)config->bootstrap_generation,
                    config->local_node, config->node_count);
    }
    for (peer = 0; !rc && peer < config->node_count; ++peer) {
        if (peer == config->local_node) continue;
        for (size_t i = 0; i < sizeof(canary_pattern); ++i)
            canary_pattern[i] =
                (uint8_t)(W5_MANAGED_CANARY_SEED_BASE + peer + i * 29U);
        runtime->open_stage = "peer-canary";
        rc = mem_service_provider_obmm_endpoint_verify_mapping(
            &runtime->provider, &peer_canaries[peer], 0,
            sizeof(canary_pattern),
            mem_service_provider_checksum64(canary_pattern,
                                            sizeof(canary_pattern)),
            config->timeout_ms);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=peer-canary rc=%d"
                    " peer=%u node=%u/%u\n",
                    rc, peer, config->local_node, config->node_count);
    }
    if (!rc) {
        runtime->open_stage = "provider-registration";
        rc = mem_service_provider_obmm_endpoint_registration(&runtime->provider,
                                                             &registration);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open"
                    " stage=provider-registration rc=%d\n", rc);
    }
    if (!rc) {
        runtime->open_stage = "provider-register";
        rc = mem_service_provider_registry_register(&runtime->registry,
                                                    &registration);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=provider-register rc=%d\n",
                    rc);
    }
    if (!rc) {
        runtime->open_stage = "channel-bind";
        rc = mem_service_provider_channel_bind(&runtime->registry,
            registration.name, registration.instance,
            MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING, &runtime->channel);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=channel-bind rc=%d\n", rc);
    }
    if (!rc) {
        runtime->open_stage = "access-domain";
        rc = mem_service_access_domain_create(&runtime->client,
                                              &runtime->channel,
                                              &runtime->domain);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=access-domain rc=%d\n",
                    rc);
    }
    if (!rc) {
        runtime->open_stage = "compute-context";
        rc = lingqu_shmem_mem_service_open_managed(&runtime->compute);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=compute-context rc=%d\n",
                    rc);
    }
    if (!rc) {
        runtime->open_stage = "pto-endpoint";
        rc = lingqu_shmem_pto_endpoint_open(&runtime->endpoint,
                                            &endpoint_info);
        if (rc != 0)
            fprintf(stderr,
                    "[w5_managed_runtime] open stage=pto-endpoint rc=%d\n",
                    rc);
    }
    if (!rc) runtime->open_stage = "ready";
    return rc;
}

const char *w5_managed_runtime_open_stage(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime && runtime->open_stage ? runtime->open_stage : "unavailable";
}

const char *w5_managed_runtime_open_detail(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime && runtime->open_detail[0] ? runtime->open_detail : "none";
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

const char *w5_managed_runtime_client_node_id(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime ? runtime->client_node_id : NULL;
}

uint64_t w5_managed_runtime_client_provider_incarnation(
    const struct w5_managed_runtime_guest *runtime)
{
    return runtime ? runtime->client_provider_incarnation : 0;
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
