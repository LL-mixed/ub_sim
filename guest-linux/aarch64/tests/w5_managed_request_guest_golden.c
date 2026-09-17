#include "w5_managed_request_guest.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NODES 3u
#define TEST_STEPS 2u

static const uint64_t test_run = UINT64_C(0x11);
static const uint64_t test_request = UINT64_C(0x22);
static const uint64_t first_step = 4;
static char states[TEST_NODES][TEST_STEPS][16];
static unsigned final_resolves;
static unsigned final_acquires;
static unsigned final_releases;
static unsigned retires;
static unsigned rolling_retires;

static int test_object_key(char *out, size_t capacity, uint64_t run,
    uint64_t request, uint32_t producer, uint64_t step)
{
    int length;

    assert(run == test_run && request == test_request);
    length = snprintf(out, capacity, "test/%016" PRIx64 "/%016" PRIx64
        "/%08" PRIx32 "/%016" PRIx64, run, request, producer, step);
    return length < 0 || (size_t)length >= capacity ? -1 : 0;
}

static void parse_key(const char *key, uint32_t *node_out, uint64_t *step_out)
{
    uint64_t run = 0;
    uint64_t request = 0;
    uint32_t node = 0;
    uint64_t step = 0;

    assert(sscanf(key, "test/%16" SCNx64 "/%16" SCNx64
        "/%8" SCNx32 "/%16" SCNx64,
        &run, &request, &node, &step) == 4);
    assert(run == test_run && request == test_request);
    assert(node < TEST_NODES && step >= first_step &&
           step < first_step + TEST_STEPS);
    *node_out = node;
    *step_out = step;
}

static uint64_t generation_for(uint32_t node, uint64_t step)
{
    return 100U + (uint64_t)node * 10U + step - first_step;
}

static void fill_allocation(struct mem_service_client_allocation *allocation,
                            const char *key)
{
    uint32_t node;
    uint64_t step;

    parse_key(key, &node, &step);
    memset(allocation, 0, sizeof(*allocation));
    strcpy(allocation->key, key);
    strcpy(allocation->state, states[node][step - first_step]);
    allocation->generation = generation_for(node, step);
    allocation->provider_backed = true;
    allocation->size_bytes = 65536;
    allocation->address_len = 65536;
    if (node == TEST_NODES - 1U && step == first_step + TEST_STEPS - 1U &&
        strcmp(allocation->state, "active") == 0) {
        allocation->holder_count = TEST_NODES;
        for (uint32_t holder = 0; holder < TEST_NODES; ++holder) {
            snprintf(allocation->holders[holder].session_id,
                sizeof(allocation->holders[holder].session_id),
                "managed-final-%08" PRIx32, holder);
            allocation->holders[holder].generation = allocation->generation;
            snprintf(allocation->holders[holder].node_id,
                sizeof(allocation->holders[holder].node_id),
                "guest-node-%u", holder);
            allocation->holders[holder].provider_incarnation = 1000 + holder;
        }
    }
}

int mem_service_client_reference_transition(
    const struct mem_service_client *client,
    const struct mem_service_reference_request *request,
    struct mem_service_client_reference_result *result_out,
    enum mem_service_wire_status *status_out)
{
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    size_t length;

    assert(client && request && result_out && status_out);
    assert(client->wire_options.timeout_ms == 1000);
    assert(client->wire_options.max_attempts == 1);
    assert(client->wire_options.retry_backoff_ms == 0);
    assert(client->wire_options.retry_on_timeout == 0);
    assert(request->action == MEM_SERVICE_REFERENCE_RESOLVE);
    length = strlen(request->key);
    assert(length > 6 && strcmp(request->key + length - 6, "/token") == 0);
    memcpy(key, request->key, length - 6);
    key[length - 6] = '\0';
    memset(result_out, 0, sizeof(*result_out));
    fill_allocation(&result_out->allocation, key);
    result_out->reference.object.magic = LINGQU_OBJECT_REF_MAGIC;
    result_out->reference.object.layout_version =
        LINGQU_OBJECT_REF_V2_LAYOUT_VERSION;
    result_out->reference.object.object_kind = 3;
    result_out->reference.object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    result_out->reference.object.owner_entity = TEST_NODES;
    result_out->reference.object.producer_entity = TEST_NODES;
    result_out->reference.object.object_version = 1;
    result_out->reference.object.payload_bytes = 64;
    result_out->reference.object.payload_checksum = 0x1234;
    result_out->reference.wire_bytes = LINGQU_OBJECT_REF_V2_BYTES;
    result_out->reference.access = LINGQU_OBJECT_REF_V2_READ;
    result_out->reference.allocation_generation =
        result_out->allocation.generation;
    result_out->reference.provider_incarnation = 1002;
    result_out->reference.allocation_bytes = result_out->allocation.size_bytes;
    strcpy(result_out->reference.allocation_key, key);
    strcpy(result_out->reference.home_node, "guest-node-2");
    result_out->reference.object.key_hash = lingqu_object_ref_key_hash(
        key, strlen(key));
    assert(lingqu_object_ref_v2_validate(&result_out->reference) == 0);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++final_resolves;
    return 0;
}

int mem_service_client_reference_transition_at_node(
    const struct mem_service_client *client,
    const struct mem_service_reference_request *request,
    const char *holder_node_id,
    uint64_t holder_provider_incarnation,
    struct mem_service_client_reference_result *result_out,
    enum mem_service_wire_status *status_out)
{
    assert(client && request && result_out && status_out);
    assert(request->action == MEM_SERVICE_REFERENCE_ACQUIRE);
    assert(request->access == LINGQU_OBJECT_REF_V2_READ);
    assert(strcmp(request->session_id, "managed-final-00000002") == 0);
    assert(strncmp(request->idempotency_key, "mfa-00000002-", 13) == 0);
    assert(strcmp(holder_node_id, "guest-node-2") == 0);
    assert(holder_provider_incarnation == 1002);
    assert(lingqu_object_ref_v2_validate(&request->reference) == 0);
    memset(result_out, 0, sizeof(*result_out));
    fill_allocation(&result_out->allocation,
                    request->reference.allocation_key);
    result_out->reference = request->reference;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++final_acquires;
    return 0;
}

int mem_service_client_release_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    const char *session_id,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    uint32_t node;
    uint64_t step;

    assert(client && key && idempotency_key && session_id && allocation_out);
    parse_key(key, &node, &step);
    assert(node == TEST_NODES - 1U && step == first_step + TEST_STEPS - 1U);
    assert(strncmp(idempotency_key, "mfl-00000002-", 13) == 0);
    assert(strcmp(session_id, "managed-final-00000002") == 0);
    assert(has_expected_generation);
    assert(expected_generation == generation_for(node, step));
    assert(strcmp(states[node][step - first_step], "retiring") == 0);
    strcpy(states[node][step - first_step], "retired");
    fill_allocation(allocation_out, key);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++final_releases;
    return 0;
}

int mem_service_client_inspect_allocation(
    const struct mem_service_client *client,
    const char *key,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    uint32_t node;
    uint64_t step;

    (void)client;
    parse_key(key, &node, &step);
    if (node == TEST_NODES - 1U && step == first_step &&
        strcmp(states[node][step - first_step], "retiring") == 0)
        strcpy(states[node][step - first_step], "retired");
    fill_allocation(allocation_out, key);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_retire_object(
    const struct mem_service_client *client,
    const char *key,
    const char *idempotency_key,
    bool has_expected_generation,
    uint64_t expected_generation,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    uint32_t node;
    uint64_t step;

    (void)client;
    parse_key(key, &node, &step);
    assert(idempotency_key && idempotency_key[0]);
    assert(has_expected_generation);
    assert(expected_generation == generation_for(node, step));
    assert(node == TEST_NODES - 1U);
    assert(strcmp(states[node][step - first_step], "active") == 0);
    if (strncmp(idempotency_key, "wrr-", 4) == 0) {
        assert(step == first_step);
        strcpy(states[node][step - first_step], "retiring");
        ++rolling_retires;
    } else {
        assert(step == first_step + TEST_STEPS - 1U);
        strcpy(states[node][step - first_step], "retiring");
    }
    fill_allocation(allocation_out, key);
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++retires;
    return 0;
}

int mem_service_client_allocation_stats(
    const struct mem_service_client *client,
    struct mem_service_client_allocation_stats *stats_out,
    enum mem_service_wire_status *status_out)
{
    (void)client;
    memset(stats_out, 0, sizeof(*stats_out));
    stats_out->backing_registered = TEST_NODES;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int main(void)
{
    struct mem_service_client client = {.connect_spec = "unix:/tmp/service"};
    struct w5_managed_request_cleanup *cleanup = NULL;
    struct w5_managed_request_cleanup_result result;
    struct mem_service_client_allocation rolled;
    struct w5_managed_request_cleanup_config config = {
        .client = &client,
        .holder_node_id = "guest-node-2",
        .holder_provider_incarnation = 1002,
        .object_key = test_object_key,
        .run = test_run,
        .request = test_request,
        .first_step = first_step,
        .step_count = TEST_STEPS,
        .timeout_ms = 1000,
        .local_node = TEST_NODES - 1U,
        .node_count = TEST_NODES,
    };

    for (uint32_t node = 0; node < TEST_NODES; ++node) {
        for (uint64_t step = 0; step < TEST_STEPS; ++step)
            strcpy(states[node][step], node == TEST_NODES - 1U ?
                   "active" : "retired");
    }
    memset(&rolled, 0, sizeof(rolled));
    assert(w5_managed_request_retire_local_step(&client, test_object_key,
        test_run, test_request, TEST_NODES - 1U, first_step, 1000,
        &rolled) == 0);
    assert(strcmp(rolled.state, "retired") == 0);
    assert(rolled.generation == generation_for(TEST_NODES - 1U,
                                                first_step));
    memset(&rolled, 0, sizeof(rolled));
    assert(w5_managed_request_retire_local_step(&client, test_object_key,
        test_run, test_request, TEST_NODES - 1U, first_step, 1000,
        &rolled) == 0);
    assert(strcmp(rolled.state, "retired") == 0 && rolling_retires == 1);
    memset(&result, 0, sizeof(result));
    assert(w5_managed_request_cleanup_create(&config, &cleanup) == 0);
    assert(cleanup);
    assert(w5_managed_request_cleanup_execute(cleanup, &result) == 0);
    assert(result.allocation_count == TEST_NODES * TEST_STEPS);
    assert(result.final_generation == generation_for(
        TEST_NODES - 1U, first_step + TEST_STEPS - 1U));
    assert(result.stats.backing_registered == TEST_NODES);
    assert(w5_managed_request_cleanup_destroy(&cleanup) == 0 && !cleanup);
    assert(final_resolves == 1 && final_acquires == 1 && final_releases == 1 &&
           retires == TEST_STEPS && rolling_retires == 1);
    puts("w5_managed_request_guest=pass rolling_retire=1 terminal_barrier=1 "
         "holder_only_barrier=1 local_retire=2 request_retired=6 "
         "resource_baseline=1");
    return 0;
}
