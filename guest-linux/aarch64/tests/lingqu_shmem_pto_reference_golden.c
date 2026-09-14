/* SPDX-License-Identifier: MIT */
#include "../apps/lingqu_shmem_pto_direct/lingqu_shmem_pto_managed.c"
#include <assert.h>

/* Application orchestration only. These stubs never connect to a service or
 * map payloads; actual provider and guest evidence must be collected separately. */
static struct mem_service_client_reference_result resolved;
static unsigned acquires, maps, lookups;
static int mapping_result, acquire_result;
static bool pending_first;

int mem_service_client_reference_transition(const struct mem_service_client *client,
    const struct mem_service_reference_request *request,
    struct mem_service_client_reference_result *result, enum mem_service_wire_status *status)
{
    (void)client;
    char payload[2048];
    assert(!mem_service_reference_format_request(request, payload, sizeof(payload)));
    if (request->action == MEM_SERVICE_REFERENCE_RESOLVE) {
        assert(!strcmp(request->key, "pto-test/input"));
        if (++lookups == 1 && pending_first) {
            *status = MEM_SERVICE_WIRE_STATUS_STALE_REF;
            return 1;
        }
    } else {
        assert(request->action == MEM_SERVICE_REFERENCE_ACQUIRE);
        assert(!strcmp(request->key, "pto-test"));
        assert(request->access == LINGQU_OBJECT_REF_V2_READ);
        assert(!memcmp(&request->reference, &resolved.reference, sizeof(request->reference)));
        ++acquires;
        if (acquire_result) { *status = MEM_SERVICE_WIRE_STATUS_INTERNAL; return acquire_result; }
    }
    *result = resolved;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int mem_service_client_map_managed_reference(const struct mem_service_client *client,
    const struct mem_service_provider_channel *channel,
    const struct lingqu_object_ref_wire_v2 *reference, const char *session, const char *operation,
    struct mem_service_client_object_mapping *mapping,
    struct mem_service_client_reference_lifecycle *lifecycle, enum mem_service_wire_status *status)
{
    (void)client; (void)channel; (void)status;
    assert(session[0] && !strncmp(operation, "pto-map-", 8) && strlen(operation) == 40);
    ++maps;
    lifecycle->reference = *reference;
    lifecycle->mapping.pending = true;
    if (mapping_result) return mapping_result;
    lifecycle->mapping.transaction.mapping_id = 42;
    lifecycle->mapping.transaction.state = 2;
    mapping->len = reference->object.payload_bytes;
    mapping->base = (void *)(uintptr_t)(resolved.allocation.address + reference->object.payload_offset);
    return 0;
}

static void initialize(struct managed_state *state, unsigned geometry)
{
    memset(state, 0, sizeof(*state));
    memset(&resolved, 0, sizeof(resolved));
    acquires = maps = lookups = 0;
    mapping_result = acquire_result = 0;
    pending_first = false;
    state->config.geometry = managed_geometries[geometry];
    state->config.timeout_ms = 200;
    state->config.node = 1;
    state->config.readonly_inputs = state->config.reference_inputs = true;
    strcpy(state->config.key, "pto-test");
    strcpy(state->session, "pto-consumer");
    state->allocation.generation = 7;
    state->allocation.provider_incarnation = 9;
    resolved.allocation = state->allocation;
    resolved.allocation.address = 0x400000;
    resolved.allocation.version = 2;
    struct lingqu_object_ref_wire_v2 *ref = &resolved.reference;
    ref->object.magic = LINGQU_OBJECT_REF_MAGIC;
    ref->object.layout_version = 2;
    ref->object.object_kind = 1;
    ref->object.state = LINGQU_OBJECT_STATE_COMMITTED_WIRE;
    ref->object.object_version = 2;
    ref->object.key_hash = lingqu_object_ref_key_hash("pto-test", 8);
    ref->object.payload_offset = vector_offset(state, 0);
    ref->object.payload_bytes = vector_offset(state, 1) +
        state->config.geometry.tensor_bytes - vector_offset(state, 0);
    ref->wire_bytes = 256;
    ref->access = LINGQU_OBJECT_REF_V2_READ;
    ref->allocation_generation = 7;
    ref->provider_incarnation = 9;
    ref->allocation_bytes = state->config.geometry.object_bytes;
    strcpy(ref->allocation_key, "pto-test");
    strcpy(ref->home_node, "guest-node-0");
}

int main(void)
{
    struct managed_state *state = calloc(1, sizeof(*state));
    assert(state);
    for (unsigned geometry = 0; geometry < 3; ++geometry) {
        initialize(state, geometry);
        pending_first = true;
        uint64_t second = vector_offset(state, 1) - vector_offset(state, 0);
        assert(!map_reference_input(state));
        assert(lookups == 2 && acquires == 1 && maps == 1 && state->held);
        assert(state->lifecycle.pending && state->lifecycle.transaction.mapping_id == 42);
        assert(!state->reference_lifecycle.mapping.pending);
        assert(!vector_offset(state, 0) && vector_offset(state, 1) == second);
        assert(mapping_access(state) == MEM_SERVICE_CLIENT_MAP_READ);
        struct managed_state output = {0};
        output.config = state->config;
        output.is_output = true;
        state->output = &output;
        assert(vector_state(state, 2) == &output);
        assert(vector_offset(&output, 2) == (geometry == 0 ? 12160 :
                                           2 * state->config.geometry.tensor_bytes));
        assert(mapping_access(&output) == (MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE));
    }
    initialize(state, 0);
    state->held = true;
    state->config.node = 0;
    state->reference = resolved.reference;
    assert(!map_reference_input(state) && !acquires && maps == 1);
    initialize(state, 0);
    resolved.reference.provider_incarnation++;
    assert(map_reference_input(state) == -ESTALE && !acquires && !maps);
    initialize(state, 0);
    resolved.reference.object.payload_offset++;
    assert(map_reference_input(state) == -ESTALE && !acquires && !maps);
    initialize(state, 0);
    acquire_result = -1;
    assert(map_reference_input(state) && acquires == 1 && !maps && !state->held);
    assert(state->reference_request.action == MEM_SERVICE_REFERENCE_ACQUIRE);
    initialize(state, 0);
    mapping_result = MEM_SERVICE_MAPPING_CLEANUP_REQUIRED;
    assert(map_reference_input(state) == mapping_result && state->held);
    assert(state->reference_lifecycle.mapping.pending && !state->lifecycle.pending);
    assert(!memcmp(&state->reference_lifecycle.reference, &resolved.reference,
                   sizeof(resolved.reference)));
    free(state);
    puts("managed PTO reference orchestration: pass");
    return 0;
}
