/* SPDX-License-Identifier: MIT */
#ifndef W5_MANAGED_OBJECT_GUEST_H
#define W5_MANAGED_OBJECT_GUEST_H

#include "components/mem_service/mem_service_mapping_owner.h"
#include "components/mem_service/lingqu_object_service.h"

#include <stddef.h>
#include <stdint.h>

struct w5_managed_object_guest;

struct w5_managed_writer_config {
    const char *key;
    const char *home_node;
    const char *session_id;
    const char *operation_prefix;
    uint64_t size_bytes;
    uint64_t timeout_ms;
    uint32_t pin_limit;
};

struct w5_managed_reader_config {
    const char *reference_key;
    const char *session_id;
    const char *operation_prefix;
    uint64_t timeout_ms;
    uint32_t pin_limit;
};

/* The deployment opens and owns client, channel and access domain. This layer
 * never selects a provider. Success transfers the service holder, managed
 * mapping and cleanup obligation into one process-local owner. An error may
 * return a cleanup-only object; retain it and retry close. */
int w5_managed_writer_open(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain *domain,
    const struct w5_managed_writer_config *config,
    struct w5_managed_object_guest **object_out,
    enum mem_service_wire_status *status_out);

int w5_managed_reader_open(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain *domain,
    const struct w5_managed_reader_config *config,
    struct w5_managed_object_guest **object_out,
    enum mem_service_wire_status *status_out);

struct mem_service_mapping_owner *w5_managed_object_owner(
    struct w5_managed_object_guest *object);

const struct mem_service_client_allocation *w5_managed_object_allocation(
    const struct w5_managed_object_guest *object);

const struct lingqu_object_ref_wire_v2 *w5_managed_object_reference(
    const struct w5_managed_object_guest *object);

int w5_managed_object_acquire_cpu(
    struct w5_managed_object_guest *object,
    uint64_t offset,
    uint64_t length,
    uint64_t flags,
    struct mem_service_mapping_lease **lease_out,
    struct mem_service_mapping_access_view *view_out);

/* Visibility operations use the same mapped owner and CPU lease. The range is
 * allocation-relative and must be contained by the lease view. */
int w5_managed_object_publish_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum);

int w5_managed_object_wait_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum);

/* Stage a view from the writer's live CPU lease. reference_key is the public
 * logical name resolved by readers (for example <allocation>/hidden). The
 * output is the exact immutable V2 reference accepted by STAGE. */
int w5_managed_writer_stage(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    const char *reference_key,
    const struct mem_service_client_reference_view *view,
    struct lingqu_object_ref_wire_v2 *reference_out,
    enum mem_service_wire_status *status_out);

/* No CPU or compute lease may remain. Unmap retains the holder, SEAL publishes
 * every staged view atomically for this allocation version, then close releases
 * the writer holder. An uncertain step retains the object for an exact retry. */
int w5_managed_writer_seal(
    struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out);

/* Readers close after the original completion. Writers close only after SEAL.
 * Success destroys the owner and frees the object. Failure retains *object. */
int w5_managed_object_close(
    struct w5_managed_object_guest **object,
    enum mem_service_wire_status *status_out);

#endif
