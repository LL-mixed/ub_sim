/* SPDX-License-Identifier: MIT */
#include "../apps/lingqu_shmem_pto_direct/lingqu_shmem_pto_managed.c"

#include <assert.h>

/* Isolate the application's guard orchestration; SDK ownership has its own
 * fixture. No device, payload mapping, thread, or network is created here. */
struct mem_service_mapping_owner {
    struct mem_service_mapping_owner_stats stats;
    int close_rc, inspect_rc, acquire_rc;
    unsigned closes, acquires;
    bool leaked_lease;
};

struct lingqu_shmem_mem_service_context { unsigned active, opens; };
static struct lingqu_shmem_mem_service_context compute;
static unsigned release_probes;

int lingqu_shmem_mem_service_open_managed(struct lingqu_shmem_mem_service_context **out)
{
    /* Match the real driver's exclusive-open contract. */
    if (compute.opens) return -EBUSY;
    ++compute.opens;
    *out = &compute;
    return 0;
}

uint32_t lingqu_shmem_mem_service_active_leases(
    const struct lingqu_shmem_mem_service_context *context)
{
    assert(context == &compute);
    return context->active;
}

int lingqu_shmem_mem_service_release(struct lingqu_shmem_mem_service_lease *lease)
{
    assert(lease);
    ++release_probes;
    return -EBUSY;
}

void lingqu_shmem_pto_dispatch_finish(struct lingqu_shmem_pto_inflight *inflight)
{
    (void)inflight;
    assert(!"pending request must not finish");
}

uint32_t lingqu_shmem_pto_endpoint_queued_count(const struct lingqu_shmem_pto_endpoint *endpoint)
{
    assert(endpoint);
    return 2;
}

int lingqu_shmem_pto_endpoint_close_checked(struct lingqu_shmem_pto_endpoint *endpoint)
{
    assert(endpoint);
    return -EBUSY;
}

int mem_service_mapping_owner_close(struct mem_service_mapping_owner *owner,
                                   enum mem_service_wire_status *status)
{
    (void)status;
    ++owner->closes;
    return owner->close_rc;
}

int mem_service_mapping_owner_inspect(struct mem_service_mapping_owner *owner,
                                     struct mem_service_mapping_owner_stats *stats)
{
    *stats = owner->stats;
    return owner->inspect_rc;
}

int mem_service_mapping_owner_acquire(struct mem_service_mapping_owner *owner,
    enum mem_service_mapping_access_kind kind, uint64_t offset, uint64_t len,
    uint64_t flags, struct mem_service_mapping_lease **lease,
    struct mem_service_mapping_access_view *view)
{
    *view = (struct mem_service_mapping_access_view) {0};
    *lease = NULL;
    assert(kind == MEM_SERVICE_MAPPING_ACCESS_CPU && offset == 3968);
    assert(len == 256 && flags == MEM_SERVICE_CLIENT_MAP_READ);
    ++owner->acquires;
    if (owner->leaked_lease) *lease = (void *)owner;
    return owner->acquire_rc;
}

static void exercise(unsigned home, bool readonly, bool pending, unsigned failure, unsigned requests)
{
    struct managed_state *input = calloc(1, sizeof(*input));
    struct managed_state *output = calloc(1, sizeof(*output));
    assert(input && output);
    struct mem_service_mapping_owner owners[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        owners[i].close_rc = -EBUSY;
        owners[i].acquire_rc = -ECANCELED;
        owners[i].stats = (struct mem_service_mapping_owner_stats) {
            .cpu_pins = 1, .closing = true, .mapping_pending = true, .holder_pending = true,
            .compute_pins = pending ? (readonly ? (i ? 2 : 1) : 3) : 0,
        };
        owners[i].stats.in_flight_pins = owners[i].stats.compute_pins;
        if (pending && requests == 2)
            owners[i].stats.compute_pins = owners[i].stats.in_flight_pins = readonly ? (i ? 2 : 4) : 6;
    }
    input->owner = &owners[0];
    input->config = (struct managed_config) {.node = 1 - home, .home_node = home, .generation = 135,
        .readonly_inputs = readonly, .geometry = managed_geometries[0], .concurrent_requests = requests};
    output->config = input->config;
    output->owner = &owners[1];
    output->is_output = true;
    if (readonly) input->output = output;
    assert(mapping_access(input) == (readonly ? MEM_SERVICE_CLIENT_MAP_READ :
        (MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE)));
    assert(mapping_access(output) == (MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE));
    input->config.node = home;
    assert(mapping_access(input) == (MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE));
    input->config.node = 1 - home;
    assert(pthread_mutex_init(&input->workers_lock, NULL) == 0);
    assert(pthread_cond_init(&input->workers_condition, NULL) == 0);
    int expected = 0;
    if (failure == 1) { owners[1].close_rc = 0; expected = -EPROTO; }
    if (failure == 2) { owners[1].inspect_rc = -EIO; expected = -EIO; }
    if (failure == 3) { owners[1].acquire_rc = 0; expected = -EPROTO; }
    if (failure == 4) { owners[1].leaked_lease = true; expected = -EPROTO; }
    if (failure == 5) { owners[1].stats.in_flight_pins++; expected = -EPROTO; }
    if (failure == 6) { input->worker_error = -EIO; expected = -EIO; }
    assert(probe_owner_close(input, pending) == expected);
    assert(owners[0].closes == 1 && owners[0].acquires == 1 && input->owner_sealed);
    if (!failure && readonly) {
        assert(owners[1].closes == 1 && owners[1].acquires == 1 && output->owner_sealed);
    }
    if (!readonly || failure == 6) assert(owners[1].closes == 0);
    assert(pthread_cond_destroy(&input->workers_condition) == 0);
    assert(pthread_mutex_destroy(&input->workers_lock) == 0);
    free(output);
    free(input);
}

static void exercise_shared_compute(void)
{
    struct managed_state parent = {0}, requests[2] = {0};
    struct managed_batch batch = {.submitted = 2, .started = 2};
    struct mem_service_mapping_owner owner = {
        .close_rc = -EBUSY, .acquire_rc = -ECANCELED,
        .stats = {.cpu_pins = 1, .compute_pins = 6, .in_flight_pins = 6,
                  .closing = true, .mapping_pending = true, .holder_pending = true},
    };
    parent.config = (struct managed_config) {.node = 1, .generation = 135,
        .geometry = managed_geometries[0], .concurrent_requests = 2};
    parent.owner = &owner;
    parent.batch = &batch;
    parent.endpoint = (void *)&batch;
    assert(pthread_mutex_init(&parent.workers_lock, NULL) == 0);
    assert(pthread_cond_init(&parent.workers_condition, NULL) == 0);
    assert(open_compute(&parent) == 0 && compute.opens == 1 && !parent.metadata);
    struct lingqu_shmem_mem_service_context *duplicate = NULL;
    assert(lingqu_shmem_mem_service_open_managed(&duplicate) == -EBUSY && !duplicate);
    for (unsigned i = 0; i < 2; ++i) {
        requests[i].compute = parent.compute;
        requests[i].completion_unknown = true;
        requests[i].metadata = (void *)(uintptr_t)((i + 1) * METADATA_BYTES);
        requests[i].metadata_iova = (i + 1) * METADATA_BYTES;
        for (unsigned j = 0; j < 3; ++j) requests[i].leases[j] = (void *)&requests[i];
        batch.requests[i] = &requests[i];
    }
    for (unsigned count = 0; count <= 7; ++count) {
        compute.active = count;
        assert(probe_batch(&parent) == (count == 6 ? 0 : -EPROTO));
    }
    assert(release_probes == 6 && compute.opens == 1);
    compute.active = 6;
    requests[1].compute = NULL;
    assert(probe_batch(&parent) == -EPROTO);
    requests[1].compute = parent.compute;
    requests[1].metadata_iova = requests[0].metadata_iova;
    assert(probe_batch(&parent) == -EPROTO);
    assert(release_probes == 6);
    assert(pthread_cond_destroy(&parent.workers_condition) == 0);
    assert(pthread_mutex_destroy(&parent.workers_lock) == 0);
    puts("managed PTO shared compute guards: pass");
}

int main(void)
{
    for (unsigned home = 0; home < 2; ++home) {
        for (unsigned requests = 1; requests <= 2; ++requests)
        for (unsigned pending = 0; pending < 2; ++pending) {
            exercise(home, false, pending, 0, requests);
            for (unsigned failure = 0; failure <= 6; ++failure)
                exercise(home, true, pending, failure, requests);
        }
    }
    exercise_shared_compute();
    puts("managed PTO owner guards: pass");
    return 0;
}
