/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "w5_managed_request_guest.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum cleanup_phase {
    CLEANUP_CREATED = 0,
    CLEANUP_FINAL_READER_OPEN = 1,
    CLEANUP_FINAL_RETIRING = 2,
    CLEANUP_FINAL_READER_CLOSED = 3,
    CLEANUP_LOCAL_RETIRE_REQUESTED = 4,
    CLEANUP_ALL_RETIRED = 5,
    CLEANUP_COMPLETE = 6,
};

struct w5_managed_request_cleanup {
    struct w5_managed_request_cleanup_config config;
    struct w5_managed_object_guest *final_reader;
    struct mem_service_client_allocation final_allocation;
    struct w5_managed_request_cleanup_result result;
    char final_key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char final_reference_key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    enum cleanup_phase phase;
};

static uint64_t now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000U +
        (uint64_t)now.tv_nsec / 1000000U;
}

static void wait_tick(void)
{
    const struct timespec delay = {.tv_nsec = 1000000};

    (void)nanosleep(&delay, NULL);
}

static uint64_t identity_hash(const char *text)
{
    uint64_t hash = UINT64_C(1469598103934665603);

    if (!text)
        return 0;
    for (const unsigned char *cursor = (const unsigned char *)text;
         *cursor; ++cursor)
        hash = (hash ^ *cursor) * UINT64_C(1099511628211);
    return hash;
}

static int final_session(char *out, size_t capacity, uint32_t node)
{
    int length;

    if (!out || !capacity)
        return -EINVAL;
    length = snprintf(out, capacity, "managed-final-%08" PRIx32, node);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

static int object_key(
    const struct w5_managed_request_cleanup *cleanup,
    uint32_t producer,
    uint64_t step,
    char *out,
    size_t capacity)
{
    if (!cleanup || !cleanup->config.object_key)
        return -EINVAL;
    return cleanup->config.object_key(out, capacity, cleanup->config.run,
        cleanup->config.request, producer, step);
}

static int open_final_reader(struct w5_managed_request_cleanup *cleanup)
{
    struct w5_managed_reader_config reader_config;
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    char session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    char operation[48];
    uint64_t hash;
    int length;
    int rc;

    rc = final_session(session, sizeof(session), cleanup->config.local_node);
    if (rc)
        return rc;
    hash = identity_hash(cleanup->final_key);
    length = snprintf(operation, sizeof(operation),
                      "mf-%08" PRIx32 "-%016" PRIx64,
                      cleanup->config.local_node, hash);
    if (length < 0 || (size_t)length >= sizeof(operation))
        return -ENAMETOOLONG;
    reader_config = (struct w5_managed_reader_config) {
        .reference_key = cleanup->final_reference_key,
        .session_id = session,
        .operation_prefix = operation,
        .timeout_ms = cleanup->config.timeout_ms,
        .pin_limit = cleanup->config.pin_limit,
    };
    rc = w5_managed_reader_open(cleanup->config.client,
        cleanup->config.channel, cleanup->config.domain, &reader_config,
        &cleanup->final_reader, &status);
    if (rc)
        return rc;
    cleanup->final_allocation =
        *w5_managed_object_allocation(cleanup->final_reader);
    if (strcmp(cleanup->final_allocation.key, cleanup->final_key) != 0 ||
        cleanup->final_allocation.generation == 0)
        return -ESTALE;
    cleanup->phase = CLEANUP_FINAL_READER_OPEN;
    return 0;
}

static bool expected_final_holders(
    const struct w5_managed_request_cleanup *cleanup,
    const struct mem_service_client_allocation *allocation)
{
    bool seen[MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS] = {false};
    uint32_t expected;

    if (!cleanup || !allocation ||
        cleanup->config.node_count > MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS ||
        allocation->holder_count != cleanup->config.node_count)
        return false;
    for (uint32_t holder = 0; holder < allocation->holder_count; ++holder) {
        bool matched = false;

        for (expected = 0; expected < cleanup->config.node_count; ++expected) {
            char session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];

            if (final_session(session, sizeof(session), expected) != 0)
                return false;
            if (strcmp(allocation->holders[holder].session_id, session) == 0) {
                if (seen[expected] ||
                    allocation->holders[holder].generation !=
                        cleanup->final_allocation.generation)
                    return false;
                seen[expected] = true;
                matched = true;
                break;
            }
        }
        if (!matched)
            return false;
    }
    for (expected = 0; expected < cleanup->config.node_count; ++expected) {
        if (!seen[expected])
            return false;
    }
    return true;
}

static int wait_final_retiring(struct w5_managed_request_cleanup *cleanup)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    uint64_t start = now_ms();

    do {
        struct mem_service_client_allocation allocation;
        int rc = mem_service_client_inspect_allocation(cleanup->config.client,
            cleanup->final_key, &allocation, &status);

        if (rc)
            return rc;
        if (allocation.generation != cleanup->final_allocation.generation)
            return -ESTALE;
        if (strcmp(allocation.state, "retiring") == 0 ||
            strcmp(allocation.state, "retired") == 0) {
            cleanup->final_allocation = allocation;
            cleanup->phase = CLEANUP_FINAL_RETIRING;
            return 0;
        }
        if (strcmp(allocation.state, "active") != 0)
            return -ESTALE;
        if (cleanup->config.local_node + 1U == cleanup->config.node_count &&
            expected_final_holders(cleanup, &allocation)) {
            struct mem_service_client_allocation retired;
            char operation[48];
            int length = snprintf(operation, sizeof(operation),
                "wfr-%016" PRIx64, identity_hash(cleanup->final_key));

            if (length < 0 || (size_t)length >= sizeof(operation))
                return -ENAMETOOLONG;
            rc = mem_service_client_retire_object(cleanup->config.client,
                cleanup->final_key, operation, true,
                cleanup->final_allocation.generation, &retired, &status);
            if (rc)
                return rc;
            if (retired.generation != cleanup->final_allocation.generation ||
                (strcmp(retired.state, "retiring") != 0 &&
                 strcmp(retired.state, "retired") != 0))
                return -ESTALE;
            cleanup->final_allocation = retired;
            cleanup->phase = CLEANUP_FINAL_RETIRING;
            return 0;
        }
        wait_tick();
    } while (now_ms() - start < cleanup->config.timeout_ms);
    return -ETIMEDOUT;
}

static int close_final_reader(struct w5_managed_request_cleanup *cleanup)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int rc;

    if (!cleanup->final_reader) {
        cleanup->phase = CLEANUP_FINAL_READER_CLOSED;
        return 0;
    }
    rc = w5_managed_object_close(&cleanup->final_reader, &status);
    if (!rc)
        cleanup->phase = CLEANUP_FINAL_READER_CLOSED;
    return rc;
}

static int request_local_retire(struct w5_managed_request_cleanup *cleanup)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    uint64_t end = cleanup->config.first_step + cleanup->config.step_count;

    for (uint64_t step = cleanup->config.first_step; step < end; ++step) {
        struct mem_service_client_allocation allocation;
        char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
        char operation[48];
        int length;
        int rc = object_key(cleanup, cleanup->config.local_node, step,
                            key, sizeof(key));

        if (rc)
            return rc;
        rc = mem_service_client_inspect_allocation(cleanup->config.client, key,
                                                   &allocation, &status);
        if (rc)
            return rc;
        if (allocation.generation == 0)
            return -ESTALE;
        if (strcmp(allocation.state, "retiring") == 0 ||
            strcmp(allocation.state, "retired") == 0)
            continue;
        if (strcmp(allocation.state, "active") != 0)
            return -ESTALE;
        length = snprintf(operation, sizeof(operation),
            "wrc-%08" PRIx32 "-%016" PRIx64,
            cleanup->config.local_node, identity_hash(key));
        if (length < 0 || (size_t)length >= sizeof(operation))
            return -ENAMETOOLONG;
        rc = mem_service_client_retire_object(cleanup->config.client, key,
            operation, true, allocation.generation, &allocation, &status);
        if (rc)
            return rc;
        if (strcmp(allocation.state, "retiring") != 0 &&
            strcmp(allocation.state, "retired") != 0)
            return -ESTALE;
    }
    cleanup->phase = CLEANUP_LOCAL_RETIRE_REQUESTED;
    return 0;
}

static int wait_all_retired(struct w5_managed_request_cleanup *cleanup)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    uint64_t start = now_ms();
    uint64_t end = cleanup->config.first_step + cleanup->config.step_count;

    do {
        bool complete = true;

        for (uint32_t node = 0; node < cleanup->config.node_count; ++node) {
            for (uint64_t step = cleanup->config.first_step; step < end; ++step) {
                struct mem_service_client_allocation allocation;
                char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
                int rc = object_key(cleanup, node, step, key, sizeof(key));

                if (rc)
                    return rc;
                rc = mem_service_client_inspect_allocation(
                    cleanup->config.client, key, &allocation, &status);
                if (rc)
                    return rc;
                if (strcmp(allocation.state, "quarantined") == 0)
                    return -EIO;
                if (strcmp(allocation.state, "retired") != 0)
                    complete = false;
            }
        }
        if (complete) {
            cleanup->phase = CLEANUP_ALL_RETIRED;
            return 0;
        }
        wait_tick();
    } while (now_ms() - start < cleanup->config.timeout_ms);
    return -ETIMEDOUT;
}

static int verify_stats(struct w5_managed_request_cleanup *cleanup)
{
    enum mem_service_wire_status status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    struct mem_service_client_allocation_stats stats;
    int rc = mem_service_client_allocation_stats(cleanup->config.client,
                                                 &stats, &status);

    if (rc)
        return rc;
    if (stats.live_objects || stats.backing_allocated_bytes ||
        stats.address_reserved_bytes || stats.export_mappings ||
        stats.import_mappings || stats.live_refs || stats.in_flight ||
        stats.quarantined_objects || stats.quarantined_bytes)
        return -EBUSY;
    cleanup->result.allocation_count =
        (uint64_t)cleanup->config.node_count * cleanup->config.step_count;
    cleanup->result.final_generation = cleanup->final_allocation.generation;
    cleanup->result.stats = stats;
    cleanup->phase = CLEANUP_COMPLETE;
    return 0;
}

int w5_managed_request_cleanup_create(
    const struct w5_managed_request_cleanup_config *config,
    struct w5_managed_request_cleanup **cleanup_out)
{
    struct w5_managed_request_cleanup *cleanup;
    uint64_t final_step;
    int length;
    int rc;

    if (!config || !cleanup_out || *cleanup_out || !config->client ||
        !config->channel || !config->domain || !config->object_key ||
        !config->run || !config->request || !config->step_count ||
        !config->timeout_ms || !config->node_count ||
        config->local_node >= config->node_count ||
        config->node_count > MEM_SERVICE_CLIENT_ALLOCATION_MAX_HOLDERS ||
        !config->pin_limit ||
        config->pin_limit > MEM_SERVICE_MAPPING_OWNER_MAX_PINS ||
        config->first_step > UINT64_MAX - config->step_count)
        return -EINVAL;
    final_step = config->first_step + config->step_count - 1U;
    cleanup = calloc(1, sizeof(*cleanup));
    if (!cleanup)
        return -ENOMEM;
    cleanup->config = *config;
    rc = config->object_key(cleanup->final_key, sizeof(cleanup->final_key),
        config->run, config->request, config->node_count - 1U, final_step);
    if (rc) {
        free(cleanup);
        return rc;
    }
    length = snprintf(cleanup->final_reference_key,
        sizeof(cleanup->final_reference_key), "%s/token", cleanup->final_key);
    if (length < 0 || (size_t)length >= sizeof(cleanup->final_reference_key)) {
        free(cleanup);
        return -ENAMETOOLONG;
    }
    *cleanup_out = cleanup;
    return 0;
}

int w5_managed_request_cleanup_execute(
    struct w5_managed_request_cleanup *cleanup,
    struct w5_managed_request_cleanup_result *result_out)
{
    int rc;

    if (!cleanup || !result_out)
        return -EINVAL;
    if (cleanup->phase < CLEANUP_FINAL_READER_OPEN) {
        rc = open_final_reader(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase < CLEANUP_FINAL_RETIRING) {
        rc = wait_final_retiring(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase < CLEANUP_FINAL_READER_CLOSED) {
        rc = close_final_reader(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase < CLEANUP_LOCAL_RETIRE_REQUESTED) {
        rc = request_local_retire(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase < CLEANUP_ALL_RETIRED) {
        rc = wait_all_retired(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase < CLEANUP_COMPLETE) {
        rc = verify_stats(cleanup);
        if (rc)
            return rc;
    }
    *result_out = cleanup->result;
    return 0;
}

int w5_managed_request_cleanup_destroy(
    struct w5_managed_request_cleanup **cleanup_ptr)
{
    struct w5_managed_request_cleanup *cleanup;
    int rc;

    if (!cleanup_ptr || !*cleanup_ptr)
        return -EINVAL;
    cleanup = *cleanup_ptr;
    if (cleanup->final_reader) {
        rc = close_final_reader(cleanup);
        if (rc)
            return rc;
    }
    if (cleanup->phase != CLEANUP_COMPLETE)
        return -EBUSY;
    free(cleanup);
    *cleanup_ptr = NULL;
    return 0;
}
