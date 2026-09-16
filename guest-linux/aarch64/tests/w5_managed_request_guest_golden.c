#include "w5_managed_request_guest.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_NODES 3u
#define TEST_STEPS 2u

struct mem_service_access_domain {
    unsigned unused;
};

struct w5_managed_object_guest {
    struct mem_service_client_allocation allocation;
};

static const uint64_t test_run = UINT64_C(0x11);
static const uint64_t test_request = UINT64_C(0x22);
static const uint64_t first_step = 4;
static char states[TEST_NODES][TEST_STEPS][16];
static unsigned reader_opens;
static unsigned reader_closes;
static unsigned retires;

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
        }
    }
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
    char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    size_t length;

    assert(client && channel && domain && config && object_out && !*object_out);
    assert(strcmp(config->session_id, "managed-final-00000002") == 0);
    assert(strncmp(config->operation_prefix, "mf-00000002-", 12) == 0);
    length = strlen(config->reference_key);
    assert(length > 6 && strcmp(config->reference_key + length - 6, "/token") == 0);
    memcpy(key, config->reference_key, length - 6);
    key[length - 6] = '\0';
    object = calloc(1, sizeof(*object));
    assert(object);
    fill_allocation(&object->allocation, key);
    *object_out = object;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++reader_opens;
    return 0;
}

const struct mem_service_client_allocation *w5_managed_object_allocation(
    const struct w5_managed_object_guest *object)
{
    return object ? &object->allocation : NULL;
}

int w5_managed_object_close(
    struct w5_managed_object_guest **object,
    enum mem_service_wire_status *status_out)
{
    uint32_t node;
    uint64_t step;

    assert(object && *object);
    parse_key((*object)->allocation.key, &node, &step);
    assert(node == TEST_NODES - 1U && step == first_step + TEST_STEPS - 1U);
    assert(strcmp(states[node][step - first_step], "retiring") == 0);
    strcpy(states[node][step - first_step], "retired");
    free(*object);
    *object = NULL;
    *status_out = MEM_SERVICE_WIRE_STATUS_OK;
    ++reader_closes;
    return 0;
}

int mem_service_client_inspect_allocation(
    const struct mem_service_client *client,
    const char *key,
    struct mem_service_client_allocation *allocation_out,
    enum mem_service_wire_status *status_out)
{
    (void)client;
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
    strcpy(states[node][step - first_step],
           step == first_step + TEST_STEPS - 1U ? "retiring" : "retired");
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
    struct mem_service_provider_channel channel = {0};
    struct mem_service_access_domain domain = {0};
    struct w5_managed_request_cleanup *cleanup = NULL;
    struct w5_managed_request_cleanup_result result;
    struct w5_managed_request_cleanup_config config = {
        .client = &client,
        .channel = &channel,
        .domain = &domain,
        .object_key = test_object_key,
        .run = test_run,
        .request = test_request,
        .first_step = first_step,
        .step_count = TEST_STEPS,
        .timeout_ms = 1000,
        .local_node = TEST_NODES - 1U,
        .node_count = TEST_NODES,
        .pin_limit = 8,
    };

    for (uint32_t node = 0; node < TEST_NODES; ++node) {
        for (uint64_t step = 0; step < TEST_STEPS; ++step)
            strcpy(states[node][step], node == TEST_NODES - 1U ?
                   "active" : "retired");
    }
    memset(&result, 0, sizeof(result));
    assert(w5_managed_request_cleanup_create(&config, &cleanup) == 0);
    assert(cleanup);
    assert(w5_managed_request_cleanup_execute(cleanup, &result) == 0);
    assert(result.allocation_count == TEST_NODES * TEST_STEPS);
    assert(result.final_generation == generation_for(
        TEST_NODES - 1U, first_step + TEST_STEPS - 1U));
    assert(result.stats.backing_registered == TEST_NODES);
    assert(w5_managed_request_cleanup_destroy(&cleanup) == 0 && !cleanup);
    assert(reader_opens == 1 && reader_closes == 1 && retires == TEST_STEPS);
    puts("w5_managed_request_guest=pass terminal_barrier=1 local_retire=2 "
         "request_retired=6 resource_baseline=1");
    return 0;
}
