/* SPDX-License-Identifier: MIT */
#ifndef W5_MANAGED_RUNTIME_GUEST_H
#define W5_MANAGED_RUNTIME_GUEST_H

#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "components/mem_service/mem_service_mapping_owner.h"

#include <stdint.h>

struct w5_managed_runtime_guest;

struct w5_managed_runtime_config {
    const char *connect_spec;
    const char *client_node_id;
    uint64_t client_provider_incarnation;
    uint64_t timeout_ms;
    uint64_t import_region_bytes;
    uint32_t max_mappings;
};

/* Provider selection is a deployment adapter detail. Model code receives only
 * this provider-neutral runtime and the ordinary Memory Service SDK objects. */
int w5_managed_runtime_open(
    const struct w5_managed_runtime_config *config,
    struct w5_managed_runtime_guest **runtime_out);

const struct mem_service_client *w5_managed_runtime_client(
    const struct w5_managed_runtime_guest *runtime);

const struct mem_service_provider_channel *w5_managed_runtime_channel(
    const struct w5_managed_runtime_guest *runtime);

struct mem_service_access_domain *w5_managed_runtime_domain(
    struct w5_managed_runtime_guest *runtime);

struct lingqu_shmem_mem_service_context *w5_managed_runtime_compute(
    struct w5_managed_runtime_guest *runtime);

struct lingqu_shmem_pto_endpoint *w5_managed_runtime_endpoint(
    struct w5_managed_runtime_guest *runtime);

const char *w5_managed_runtime_client_node_id(
    const struct w5_managed_runtime_guest *runtime);

uint64_t w5_managed_runtime_client_provider_incarnation(
    const struct w5_managed_runtime_guest *runtime);

/* All objects and callable operations must be drained first. Failure retains
 * *runtime for an exact cleanup retry. */
int w5_managed_runtime_close(struct w5_managed_runtime_guest **runtime);

#endif
