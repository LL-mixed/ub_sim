/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "w5_managed_object_guest.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W5_MANAGED_DISCOVERY_ATTEMPT_TIMEOUT_MS 5000U
#define W5_MANAGED_POLL_INITIAL_BACKOFF_MS 1U
#define W5_MANAGED_POLL_MAX_BACKOFF_MS 1000U

struct w5_managed_object_guest {
    const struct mem_service_client *client;
    const struct mem_service_provider_channel *channel;
    struct mem_service_access_domain *domain;
    struct mem_service_client_allocation allocation;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle lifecycle;
    struct mem_service_client_reference_lifecycle reference_lifecycle;
    struct mem_service_mapping_owner *owner;
    struct lingqu_object_ref_wire_v2 reference;
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    char session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    char holder_node_id[MEM_SERVICE_CLIENT_PROVIDER_NODE_ID_LEN];
    char operation_prefix[48];
    uint64_t holder_provider_incarnation;
    uint64_t timeout_ms;
    uint32_t pin_limit;
    bool writer;
    bool allocation_requested;
    bool holder_owned;
    bool reference_mapping;
    bool begun;
    bool staged;
    bool unmapped;
    bool sealed;
};

static uint64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static void wait_tick(uint64_t *backoff_ms)
{
    uint64_t current;
    struct timespec delay;
    if (!backoff_ms) return;
    current = *backoff_ms ? *backoff_ms : W5_MANAGED_POLL_INITIAL_BACKOFF_MS;
    if (current > W5_MANAGED_POLL_MAX_BACKOFF_MS)
        current = W5_MANAGED_POLL_MAX_BACKOFF_MS;
    delay.tv_sec = (time_t)(current / 1000U);
    delay.tv_nsec = (long)((current % 1000U) * 1000000U);
    (void)nanosleep(&delay, NULL);
    *backoff_ms = current < W5_MANAGED_POLL_MAX_BACKOFF_MS / 2U ?
        current * 2U : W5_MANAGED_POLL_MAX_BACKOFF_MS;
}

static int copy_text(char *out, size_t capacity, const char *text)
{
    size_t length;
    if (!out || !capacity || !text || !text[0]) return -EINVAL;
    length = strlen(text);
    if (length >= capacity) return -ENAMETOOLONG;
    memcpy(out, text, length + 1);
    return 0;
}

static int operation_id(const struct w5_managed_object_guest *object,
    const char *suffix, char *out, size_t capacity)
{
    int length;
    if (!object || !suffix || !out) return -EINVAL;
    length = snprintf(out, capacity, "%s-%s", object->operation_prefix, suffix);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

static struct w5_managed_object_guest *create_object(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain *domain,
    const char *session,
    const char *holder_node_id,
    uint64_t holder_provider_incarnation,
    const char *operation_prefix,
    uint64_t timeout_ms,
    uint32_t pin_limit)
{
    struct w5_managed_object_guest *object;
    if (!client || !channel || !domain || !session || !holder_node_id ||
        !holder_provider_incarnation || !operation_prefix ||
        !timeout_ms || !pin_limit ||
        pin_limit > MEM_SERVICE_MAPPING_OWNER_MAX_PINS) return NULL;
    object = calloc(1, sizeof(*object));
    if (!object) return NULL;
    object->client = client;
    object->channel = channel;
    object->domain = domain;
    object->timeout_ms = timeout_ms;
    object->pin_limit = pin_limit;
    object->holder_provider_incarnation = holder_provider_incarnation;
    if (copy_text(object->session, sizeof(object->session), session) ||
        copy_text(object->holder_node_id, sizeof(object->holder_node_id),
                  holder_node_id) ||
        copy_text(object->operation_prefix, sizeof(object->operation_prefix),
                  operation_prefix)) {
        free(object);
        return NULL;
    }
    return object;
}

static int transition(struct w5_managed_object_guest *object,
    uint64_t action, const char *key,
    const struct lingqu_object_ref_wire_v2 *reference,
    struct mem_service_client_reference_result *result_out,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_reference_request request = {0};
    struct mem_service_client_reference_result result;
    struct mem_service_client discovery_client;
    const struct mem_service_client *transition_client = object->client;
    char suffix[48];
    int rc;
    if (!object || !key || !result_out || !status_out) return -EINVAL;
    request.action = action;
    rc = copy_text(request.key, sizeof(request.key), key);
    if (rc) return rc;
    if (action != MEM_SERVICE_REFERENCE_RESOLVE) {
        rc = copy_text(request.session_id, sizeof(request.session_id), object->session);
        if (rc) return rc;
        int length = action == MEM_SERVICE_REFERENCE_STAGE ?
            snprintf(suffix, sizeof(suffix), "ref-%" PRIu64 "-%016" PRIx64,
                action, lingqu_object_ref_key_hash(key, strlen(key))) :
            snprintf(suffix, sizeof(suffix), "ref-%" PRIu64, action);
        if (length < 0 || (size_t)length >= sizeof(suffix)) return -ENAMETOOLONG;
        rc = operation_id(object, suffix, request.idempotency_key,
                          sizeof(request.idempotency_key));
        if (rc) return rc;
    }
    if (action == MEM_SERVICE_REFERENCE_BEGIN ||
        action == MEM_SERVICE_REFERENCE_SEAL) {
        request.generation = object->allocation.generation;
        request.version = object->allocation.version;
    }
    if (reference) request.reference = *reference;
    if (action == MEM_SERVICE_REFERENCE_ACQUIRE)
        request.access = LINGQU_OBJECT_REF_V2_READ;
    if (action == MEM_SERVICE_REFERENCE_RESOLVE) {
        discovery_client = *object->client;
        discovery_client.wire_options.timeout_ms =
            object->timeout_ms < W5_MANAGED_DISCOVERY_ATTEMPT_TIMEOUT_MS ?
            object->timeout_ms : W5_MANAGED_DISCOVERY_ATTEMPT_TIMEOUT_MS;
        discovery_client.wire_options.max_attempts = 1;
        discovery_client.wire_options.retry_backoff_ms = 0;
        discovery_client.wire_options.retry_on_timeout = 0;
        transition_client = &discovery_client;
    }
    memset(&result, 0, sizeof(result));
    *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    rc = action == MEM_SERVICE_REFERENCE_ACQUIRE ?
        mem_service_client_reference_transition_at_node(transition_client,
            &request, object->holder_node_id,
            object->holder_provider_incarnation, &result, status_out) :
        mem_service_client_reference_transition(transition_client, &request,
                                                 &result, status_out);
    if (!rc) *result_out = result;
    return rc;
}

static int wait_allocation_active(struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out)
{
    uint64_t start = now_ms();
    uint64_t backoff_ms = W5_MANAGED_POLL_INITIAL_BACKOFF_MS;
    do {
        struct mem_service_client_allocation allocation;
        int rc = mem_service_client_inspect_allocation(object->client, object->key,
                                                       &allocation, status_out);
        if (!rc) {
            object->allocation = allocation;
            if (!strcmp(allocation.state, "active")) return 0;
            if (!strcmp(allocation.state, "quarantined") ||
                !strcmp(allocation.state, "retired")) return -ESTALE;
        } else if (*status_out != MEM_SERVICE_WIRE_STATUS_NOT_FOUND) {
            return rc;
        }
        wait_tick(&backoff_ms);
    } while (now_ms() - start < object->timeout_ms);
    return -ETIMEDOUT;
}

static int adopt(struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out)
{
    char release_operation[80];
    int rc = operation_id(object, "release", release_operation,
                          sizeof(release_operation));
    if (rc) return rc;
    rc = mem_service_mapping_owner_adopt(object->domain, &object->mapping,
        &object->lifecycle, release_operation, object->pin_limit,
        &object->owner, status_out);
    if (!rc) object->holder_owned = false;
    return rc;
}

static int report_open_failure(const struct w5_managed_object_guest *object,
    const char *role, const char *stage, int rc,
    enum mem_service_wire_status status)
{
    fprintf(stderr,
            "[w5_managed_object] role=%s stage=%s rc=%d status=%u key=%s\n",
            role, stage, rc, (unsigned)status,
            object && object->key[0] ? object->key : "none");
    return rc;
}

int w5_managed_writer_open(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain *domain,
    const struct w5_managed_writer_config *config,
    struct w5_managed_object_guest **object_out,
    enum mem_service_wire_status *status_out)
{
    struct w5_managed_object_guest *object;
    struct mem_service_client_allocate request = {0};
    struct mem_service_client_reference_result result;
    char idempotency[80], acquire_operation[80], map_operation[80];
    int rc;
    if (!config || !object_out || !status_out || !config->size_bytes ||
        !config->home_node || !config->home_node[0] ||
        !config->holder_node_id || !config->holder_node_id[0] ||
        !config->holder_provider_incarnation) return -EINVAL;
    *object_out = NULL;
    object = create_object(client, channel, domain, config->session_id,
        config->holder_node_id, config->holder_provider_incarnation,
        config->operation_prefix, config->timeout_ms, config->pin_limit);
    if (!object) return -EINVAL;
    *object_out = object;
    object->writer = true;
    rc = copy_text(object->key, sizeof(object->key), config->key);
    if (rc) return rc;
    rc = operation_id(object, "allocate", idempotency, sizeof(idempotency));
    if (rc) return rc;
    request.key = object->key;
    request.idempotency_key = idempotency;
    request.session_id = object->session;
    request.size_bytes = config->size_bytes;
    request.capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP;
    object->allocation_requested = true;
    rc = mem_service_client_allocate_object_at_home(client, &request,
        config->home_node, &object->allocation, status_out);
    if (rc)
        return report_open_failure(object, "writer", "allocate", rc,
                                   *status_out);
    rc = wait_allocation_active(object, status_out);
    if (rc)
        return report_open_failure(object, "writer", "wait-active", rc,
                                   *status_out);
    rc = operation_id(object, "acquire", acquire_operation,
                      sizeof(acquire_operation));
    if (rc) return rc;
    rc = mem_service_client_acquire_object_at_node(client, object->key,
        acquire_operation, object->session, object->holder_node_id,
        object->holder_provider_incarnation, true,
        object->allocation.generation, &object->allocation, status_out);
    if (rc)
        return report_open_failure(object, "writer", "holder-acquire", rc,
                                   *status_out);
    object->holder_owned = true;
    rc = transition(object, MEM_SERVICE_REFERENCE_BEGIN, object->key, NULL,
                    &result, status_out);
    if (rc)
        return report_open_failure(object, "writer", "reference-begin", rc,
                                   *status_out);
    object->allocation = result.allocation;
    object->begun = true;
    rc = operation_id(object, "map", map_operation, sizeof(map_operation));
    if (rc) return rc;
    rc = mem_service_client_map_managed_allocation(client, channel,
        &object->allocation, object->session, map_operation,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &object->mapping, &object->lifecycle, status_out);
    if (rc)
        return report_open_failure(object, "writer", "map", rc,
                                   *status_out);
    rc = adopt(object, status_out);
    return rc ? report_open_failure(object, "writer", "adopt", rc,
                                    *status_out) : 0;
}

int w5_managed_reader_open(
    const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    struct mem_service_access_domain *domain,
    const struct w5_managed_reader_config *config,
    struct w5_managed_object_guest **object_out,
    enum mem_service_wire_status *status_out)
{
    struct w5_managed_object_guest *object;
    struct mem_service_client_reference_result result;
    char map_operation[80];
    uint64_t start;
    uint64_t backoff_ms = W5_MANAGED_POLL_INITIAL_BACKOFF_MS;
    int rc;
    if (!config || !object_out || !status_out || !config->holder_node_id ||
        !config->holder_node_id[0] || !config->holder_provider_incarnation)
        return -EINVAL;
    *object_out = NULL;
    object = create_object(client, channel, domain, config->session_id,
        config->holder_node_id, config->holder_provider_incarnation,
        config->operation_prefix, config->timeout_ms, config->pin_limit);
    if (!object) return -EINVAL;
    *object_out = object;
    start = now_ms();
    do {
        rc = transition(object, MEM_SERVICE_REFERENCE_RESOLVE,
            config->reference_key, NULL, &result, status_out);
        if (!rc) break;
        if (*status_out != MEM_SERVICE_WIRE_STATUS_NOT_FOUND &&
            *status_out != MEM_SERVICE_WIRE_STATUS_TIMEOUT &&
            *status_out != MEM_SERVICE_WIRE_STATUS_STALE_REF) return rc;
        wait_tick(&backoff_ms);
    } while (now_ms() - start < object->timeout_ms);
    if (rc) return -ETIMEDOUT;
    if (lingqu_object_ref_v2_validate(&result.reference) ||
        result.reference.access != LINGQU_OBJECT_REF_V2_READ) return -ESTALE;
    object->reference = result.reference;
    object->allocation = result.allocation;
    rc = copy_text(object->key, sizeof(object->key),
                   object->reference.allocation_key);
    if (rc) return rc;
    rc = transition(object, MEM_SERVICE_REFERENCE_ACQUIRE, object->key,
        &object->reference, &result, status_out);
    if (rc) return rc;
    object->allocation = result.allocation;
    object->holder_owned = true;
    rc = operation_id(object, "map", map_operation, sizeof(map_operation));
    if (rc) return rc;
    rc = mem_service_client_map_managed_reference_at_node(client, channel,
        &object->reference, object->session, map_operation,
        object->holder_node_id, object->holder_provider_incarnation,
        &object->mapping, &object->reference_lifecycle, status_out);
    if (rc) return rc;
    object->reference_mapping = true;
    object->lifecycle = object->reference_lifecycle.mapping;
    rc = adopt(object, status_out);
    if (!rc)
        memset(&object->reference_lifecycle, 0,
               sizeof(object->reference_lifecycle));
    return rc;
}

struct mem_service_mapping_owner *w5_managed_object_owner(
    struct w5_managed_object_guest *object)
{
    return object ? object->owner : NULL;
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
    if (!object || !object->owner) return -EINVAL;
    return mem_service_mapping_owner_acquire(object->owner,
        MEM_SERVICE_MAPPING_ACCESS_CPU, offset, length, flags,
        lease_out, view_out);
}

struct visibility_context {
    uint64_t offset;
    uint64_t length;
    uint64_t checksum;
    uint64_t timeout_ms;
    bool wait;
};

static int visibility_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct visibility_context *request = opaque;
    struct mem_service_visibility_completion completion;
    uint64_t view_end;
    (void)lifecycle;
    if (!channel || !mapping || !view ||
        view->offset > UINT64_MAX - view->len ||
        request->offset > UINT64_MAX - request->length)
        return -EINVAL;
    view_end = view->offset + view->len;
    if (request->offset < view->offset ||
        request->offset + request->length > view_end)
        return -ERANGE;
    if (request->wait)
        return mem_service_provider_channel_wait_range_visible(channel,
            &mapping->binding, request->offset, request->length,
            request->checksum, request->timeout_ms, &completion);
    return mem_service_provider_channel_publish_range(channel,
        &mapping->binding, request->offset, request->length,
        request->checksum, &completion);
}

static int object_visibility(struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    uint64_t offset, uint64_t length, uint64_t checksum, bool wait)
{
    struct visibility_context request = {
        .offset = offset, .length = length, .checksum = checksum,
        .timeout_ms = object ? object->timeout_ms : 0, .wait = wait,
    };
    if (!object || !object->owner || !cpu_lease || !length) return -EINVAL;
    return mem_service_mapping_lease_apply(cpu_lease, visibility_callback,
                                           &request);
}

int w5_managed_object_publish_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum)
{
    return object_visibility(object, cpu_lease, offset, length, checksum, false);
}

int w5_managed_object_wait_range(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    uint64_t offset,
    uint64_t length,
    uint64_t checksum)
{
    return object_visibility(object, cpu_lease, offset, length, checksum, true);
}

struct stage_context {
    struct w5_managed_object_guest *object;
    const char *reference_key;
    const struct mem_service_client_reference_view *view;
    struct lingqu_object_ref_wire_v2 reference;
    enum mem_service_wire_status status;
};

static int report_stage_failure(
    const struct stage_context *stage,
    const char *phase,
    int rc)
{
    struct mem_service_client_allocation current = {0};
    enum mem_service_wire_status inspect_status =
        MEM_SERVICE_WIRE_STATUS_INTERNAL;
    int inspect_rc = mem_service_client_inspect_allocation(
        stage->object->client, stage->object->allocation.key,
        &current, &inspect_status);

    fprintf(stderr,
            "[w5_managed_object] role=writer stage=%s rc=%d status=%u"
            " snapshot_generation=%" PRIu64
            " snapshot_version=%" PRIu64
            " current_rc=%d current_status=%u"
            " current_generation=%" PRIu64
            " current_version=%" PRIu64
            " current_state=%s key=%s\n",
            phase, rc, (unsigned)stage->status,
            stage->object->allocation.generation,
            stage->object->allocation.version,
            inspect_rc, (unsigned)inspect_status,
            current.generation, current.version,
            inspect_rc == 0 ? current.state : "unavailable",
            stage->object->allocation.key);
    return rc;
}

static int stage_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *access)
{
    struct stage_context *stage = opaque;
    struct mem_service_client_reference_result result;
    int rc;
    (void)access;
    rc = mem_service_client_prepare_managed_reference(stage->object->client,
        channel, &stage->object->allocation, mapping, lifecycle, stage->view,
        &stage->reference, &stage->status);
    if (rc)
        return report_stage_failure(stage, "reference-prepare", rc);
    rc = transition(stage->object, MEM_SERVICE_REFERENCE_STAGE,
        stage->reference_key, &stage->reference, &result, &stage->status);
    if (rc)
        return report_stage_failure(stage, "reference-transition", rc);
    if (!rc) stage->object->allocation = result.allocation;
    return rc;
}

int w5_managed_writer_stage(
    struct w5_managed_object_guest *object,
    struct mem_service_mapping_lease *cpu_lease,
    const char *reference_key,
    const struct mem_service_client_reference_view *view,
    struct lingqu_object_ref_wire_v2 *reference_out,
    enum mem_service_wire_status *status_out)
{
    struct stage_context stage = {
        .object = object, .reference_key = reference_key, .view = view,
        .status = MEM_SERVICE_WIRE_STATUS_INTERNAL,
    };
    int rc;
    if (!object || !object->writer || !object->owner || !object->begun ||
        object->unmapped || object->sealed || !cpu_lease || !reference_key ||
        !reference_key[0] || !view || !reference_out || !status_out)
        return -EINVAL;
    rc = mem_service_mapping_lease_apply(cpu_lease, stage_callback, &stage);
    *status_out = stage.status;
    if (rc) return rc;
    object->staged = true;
    *reference_out = stage.reference;
    return 0;
}

int w5_managed_writer_seal(
    struct w5_managed_object_guest *object,
    enum mem_service_wire_status *status_out)
{
    struct mem_service_client_reference_result result;
    int rc;
    if (!object || !object->writer || !object->owner || !object->begun ||
        !object->staged || !status_out) return -EINVAL;
    if (!object->unmapped) {
        rc = mem_service_mapping_owner_unmap(object->owner, status_out);
        if (rc) return rc;
        object->unmapped = true;
    }
    if (!object->sealed) {
        rc = transition(object, MEM_SERVICE_REFERENCE_SEAL, object->key, NULL,
                        &result, status_out);
        if (rc) return rc;
        object->allocation = result.allocation;
        object->sealed = true;
    }
    rc = mem_service_mapping_owner_close(object->owner, status_out);
    if (rc) return rc;
    rc = mem_service_mapping_owner_destroy(object->owner);
    if (!rc) object->owner = NULL;
    return rc;
}

int w5_managed_writer_abort(
    struct w5_managed_object_guest **object_ptr,
    enum mem_service_wire_status *status_out)
{
    struct w5_managed_object_guest *object;
    struct mem_service_client_allocation retired;
    char retire_operation[80];
    int rc;

    if (!object_ptr || !*object_ptr || !status_out)
        return -EINVAL;
    object = *object_ptr;
    if (!object->writer || object->sealed)
        return -EINVAL;
    if (!object->allocation_requested) {
        free(object);
        *object_ptr = NULL;
        *status_out = MEM_SERVICE_WIRE_STATUS_OK;
        return 0;
    }
    if (object->allocation.generation == 0)
        return -EAGAIN;
    rc = operation_id(object, "abort-retire", retire_operation,
                      sizeof(retire_operation));
    if (rc)
        return rc;
    rc = mem_service_client_retire_object(object->client, object->key,
        retire_operation, true,
        object->allocation.generation, &retired, status_out);
    if (rc)
        return rc;
    object->allocation = retired;
    if (object->owner) {
        rc = mem_service_mapping_owner_close(object->owner, status_out);
        if (rc)
            return rc;
        rc = mem_service_mapping_owner_destroy(object->owner);
        if (rc)
            return rc;
        object->owner = NULL;
    }
    if (object->lifecycle.pending || object->mapping.binding.mapped) {
        rc = mem_service_client_unmap_managed_allocation(object->client,
            object->channel, &object->mapping, &object->lifecycle, status_out);
        if (rc)
            return rc;
    }
    if (object->holder_owned) {
        char release_operation[80];

        rc = operation_id(object, "release", release_operation,
                          sizeof(release_operation));
        if (rc)
            return rc;
        rc = mem_service_client_release_object(object->client, object->key,
            release_operation, object->session, true,
            object->allocation.generation, &object->allocation, status_out);
        if (rc)
            return rc;
        object->holder_owned = false;
    }
    free(object);
    *object_ptr = NULL;
    return 0;
}

int w5_managed_object_close(
    struct w5_managed_object_guest **object_ptr,
    enum mem_service_wire_status *status_out)
{
    struct w5_managed_object_guest *object;
    char release_operation[80];
    int rc;
    if (!object_ptr || !*object_ptr || !status_out) return -EINVAL;
    object = *object_ptr;
    if (object->writer && object->begun && !object->sealed) return -EBUSY;
    if (object->owner) {
        rc = mem_service_mapping_owner_close(object->owner, status_out);
        if (rc) return rc;
        rc = mem_service_mapping_owner_destroy(object->owner);
        if (rc) return rc;
        object->owner = NULL;
    }
    if (object->lifecycle.pending || object->mapping.binding.mapped) {
        rc = object->reference_mapping ?
            mem_service_client_unmap_managed_reference_at_node(object->client,
                object->channel, &object->mapping, &object->reference_lifecycle,
                object->holder_node_id,
                object->holder_provider_incarnation, status_out) :
            mem_service_client_unmap_managed_allocation(object->client,
                object->channel, &object->mapping, &object->lifecycle, status_out);
        if (rc) return rc;
    }
    if (object->holder_owned) {
        rc = operation_id(object, "release", release_operation,
                          sizeof(release_operation));
        if (rc) return rc;
        rc = mem_service_client_release_object(object->client, object->key,
            release_operation, object->session, true,
            object->allocation.generation, &object->allocation, status_out);
        if (rc) return rc;
        object->holder_owned = false;
    }
    free(object);
    *object_ptr = NULL;
    return 0;
}

int w5_managed_object_close_bounded(
    struct w5_managed_object_guest **object_ptr,
    enum mem_service_wire_status *status_out,
    uint64_t timeout_ms,
    uint32_t *retry_count_out)
{
    uint64_t backoff_ms = W5_MANAGED_POLL_INITIAL_BACKOFF_MS;
    uint64_t started_ms;
    uint32_t retries = 0;
    int rc;

    if (!object_ptr || !*object_ptr || !status_out || !timeout_ms)
        return -EINVAL;
    if (retry_count_out)
        *retry_count_out = 0;
    started_ms = now_ms();
    do {
        rc = w5_managed_object_close(object_ptr, status_out);
        if (rc != MEM_SERVICE_MAPPING_CLEANUP_REQUIRED)
            break;
        if (now_ms() - started_ms >= timeout_ms)
            break;
        wait_tick(&backoff_ms);
        ++retries;
    } while (now_ms() - started_ms < timeout_ms);
    if (retry_count_out)
        *retry_count_out = retries;
    return rc;
}
