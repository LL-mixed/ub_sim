/* SPDX-License-Identifier: MIT */
#ifndef W5_MANAGED_REQUEST_GUEST_H
#define W5_MANAGED_REQUEST_GUEST_H

#include "w5_managed_object_guest.h"

#include <stddef.h>
#include <stdint.h>

typedef int (*w5_managed_request_object_key_fn)(
    char *out,
    size_t capacity,
    uint64_t run,
    uint64_t request,
    uint32_t producer,
    uint64_t step);

struct w5_managed_request_cleanup_config {
    const struct mem_service_client *client;
    const struct mem_service_provider_channel *channel;
    struct mem_service_access_domain *domain;
    const char *holder_node_id;
    uint64_t holder_provider_incarnation;
    w5_managed_request_object_key_fn object_key;
    uint64_t run;
    uint64_t request;
    uint64_t first_step;
    uint64_t step_count;
    uint64_t timeout_ms;
    uint32_t local_node;
    uint32_t node_count;
    uint32_t pin_limit;
};

struct w5_managed_request_cleanup_result {
    uint64_t allocation_count;
    uint64_t final_generation;
    struct mem_service_client_allocation_stats stats;
};

struct w5_managed_request_cleanup;

/* Retire one allocation produced by the local node after every consumer has
 * advanced beyond that step. Success means the provider has reclaimed the
 * backing and the allocation is durably RETIRED; repeating the same call is
 * idempotent. Keeping only the immediately previous decode step bounds home
 * backing and GSVA registrations for long-running requests. */
int w5_managed_request_retire_local_step(
    const struct mem_service_client *client,
    w5_managed_request_object_key_fn object_key,
    uint64_t run,
    uint64_t request,
    uint32_t local_node,
    uint64_t step,
    uint64_t timeout_ms,
    struct mem_service_client_allocation *allocation_out);

/* Create local cleanup state without issuing an RPC. The final token is used
 * as a request-scoped rendezvous: every node acquires it, the terminal node
 * verifies the exact participant holder set and retires it, then every node
 * releases its holder and retires the allocations produced by its home.
 * Execute is retryable on the same state. */
int w5_managed_request_cleanup_create(
    const struct w5_managed_request_cleanup_config *config,
    struct w5_managed_request_cleanup **cleanup_out);

int w5_managed_request_cleanup_execute(
    struct w5_managed_request_cleanup *cleanup,
    struct w5_managed_request_cleanup_result *result_out);

/* Success requires any retained final-token mapping and holder to be closed.
 * A cleanup failure retains *cleanup for an exact retry. */
int w5_managed_request_cleanup_destroy(
    struct w5_managed_request_cleanup **cleanup);

#endif
