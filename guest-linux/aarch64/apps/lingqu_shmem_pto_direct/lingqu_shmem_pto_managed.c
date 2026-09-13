/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_pto_managed.h"
#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "lingqu_shmem_sim.h"
#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_mapping_owner.h"
#include "components/mem_service/providers/mem_service_provider_obmm.h"

#include <errno.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MANAGED_BYTES 65536U
#define VECTOR_ELEMENTS 64U
#define VECTOR_BYTES (VECTOR_ELEMENTS * sizeof(float))
#define METADATA_BYTES 4096U
#define MAX_ACCESS_THREADS 8U
#define NEGATIVE_CASES 4U

static const char *const negative_names[NEGATIVE_CASES] = {
    "bad-mapping-ref", "wrong-requester", "oob", "role-access-mismatch"
};

struct managed_config {
    char connect[128], key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint64_t generation, fingerprint, timeout_ms, submit_timeout_ms;
    uint32_t node, requester, access_threads;
    bool recovery_expected;
    bool negative_checks;
};

struct managed_state {
    struct managed_config config;
    struct mem_service_client client;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_obmm_endpoint provider;
    struct mem_service_client_allocation allocation;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle lifecycle;
    struct mem_service_access_domain *domain;
    struct mem_service_mapping_owner *owner;
    struct mem_service_mapping_lease *cpu_pin;
    uint64_t mapping_id;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_mem_service_lease *leases[3];
    struct lingqu_shmem_pto_endpoint *endpoint;
    struct lingqu_shmem_pto_inflight *inflight;
    void *metadata;
    char session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    bool held, completion_unknown;
    pthread_mutex_t workers_lock;
    pthread_cond_t workers_condition;
    pthread_t workers[MAX_ACCESS_THREADS];
    unsigned workers_started, workers_ready, worker_checks, workers_observed_seal;
    bool workers_stop, owner_sealed;
    int worker_error;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int parse_number(const char *text, uint64_t *out)
{
    char *end;
    if (!text[0] || text[0] == '-' || text[0] == '+' || text[0] == ' ')
        return -EINVAL;
    errno = 0;
    *out = strtoull(text, &end, 0);
    return errno || *end ? -EINVAL : 0;
}

static bool valid_connect(const char *spec)
{
    if (!strncmp(spec, "unix:/", 6))
        return strlen(spec + 5) < sizeof(((struct sockaddr_un *)0)->sun_path);
    if (strncmp(spec, "tcp:", 4)) return false;
    char address[INET_ADDRSTRLEN];
    const char *port = strrchr(spec + 4, ':');
    if (!port || port == spec + 4 || (size_t)(port - spec - 4) >= sizeof(address))
        return false;
    size_t size = (size_t)(port - spec - 4);
    memcpy(address, spec + 4, size);
    address[size] = '\0';
    struct in_addr parsed;
    if (inet_pton(AF_INET, address, &parsed) != 1 || !port[1]) return false;
    uint64_t value = 0;
    for (++port; *port; ++port) {
        if (*port < '0' || *port > '9' || value > 65535) return false;
        value = value * 10 + (unsigned)(*port - '0');
    }
    return value > 0 && value <= 65535;
}

static int read_config(const char *path, struct managed_config *config)
{
    const char *names[] = {"connect", "key", "node_id", "generation",
        "requester_cna", "artifact_fingerprint", "timeout_ms", "submit_timeout_ms",
        "access_threads", "negative_checks"};
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    unsigned seen = 0;
    FILE *file = fopen(path, "r");
    int rc = -EINVAL;
    if (!file) return -errno;
    memset(config, 0, sizeof(*config));
    while ((length = getline(&line, &capacity, file)) >= 0) {
        size_t n = (size_t)length;
        if (!n || n >= 512 || memchr(line, '\0', n)) goto out;
        if (line[n - 1] == '\n') line[--n] = '\0';
        if (n && line[n - 1] == '\r') line[--n] = '\0';
        if (strpbrk(line, "\r\n")) goto out;
        if (!line[0] || line[0] == '#') continue;
        char *value = strchr(line, '=');
        if (!value || !value[1]) goto out;
        *value++ = '\0';
        unsigned field;
        for (field = 0; field < sizeof(names) / sizeof(names[0]); ++field)
            if (!strcmp(line, names[field])) break;
        if (field == sizeof(names) / sizeof(names[0]) || (seen & (1U << field)))
            goto out;
        seen |= 1U << field;
        if (field < 2) {
            char *dest = field ? config->key : config->connect;
            size_t capacity = field ? sizeof(config->key) : sizeof(config->connect);
            if (strlen(value) >= capacity || strpbrk(value, " \t")) goto out;
            memcpy(dest, value, strlen(value) + 1);
        } else {
            uint64_t number;
            if (parse_number(value, &number)) goto out;
            switch (field) {
            case 2: if (number > 1) goto out; config->node = (uint32_t)number; break;
            case 3: if (!number || number > UINT64_MAX - 2) goto out;
                    config->generation = number; break;
            case 4: if (!number || number > UINT32_MAX) goto out;
                    config->requester = (uint32_t)number; break;
            case 5: if (!number) goto out; config->fingerprint = number; break;
            case 6: if (!number || number > 600000) goto out;
                    config->timeout_ms = number; break;
            case 7: if (!number || number > 600000) goto out;
                    config->submit_timeout_ms = number; break;
            case 8: if (number > MAX_ACCESS_THREADS) goto out;
                    config->access_threads = (uint32_t)number; break;
            case 9: if (number > 1) goto out;
                    config->negative_checks = number != 0; break;
            default: goto out;
            }
        }
    }
    if (ferror(file) || (seen & 127) != 127) goto out;
    if (!valid_connect(config->connect)) goto out;
    config->recovery_expected = (seen & 128) != 0;
    if (config->negative_checks && (config->recovery_expected ||
        config->generation > UINT64_MAX - 2 - NEGATIVE_CASES)) goto out;
    if (config->recovery_expected) {
        if (config->submit_timeout_ms >= config->timeout_ms) goto out;
    } else {
        config->submit_timeout_ms = config->timeout_ms;
    }
    rc = 0;
out:
    free(line);
    fclose(file);
    return rc;
}

static uint64_t vector_offset(unsigned vector)
{
    return (uint64_t)(vector + 1) * 4096 - VECTOR_BYTES / 2;
}

static float expected_value(unsigned vector, unsigned element)
{
    float a = (float)(element % 8 + 1), b = (float)(element % 4 + 1);
    if (vector == 0) return a;
    if (vector == 1) return b;
    float c = (a + b + 1.0f) * (a + b + 2.0f);
    return vector == 2 ? c : (c + b + 1.0f) * (c + b + 2.0f);
}

struct vector_check {
    struct managed_state *state;
    unsigned vector;
    const char *path;
    uint64_t negative_operation;
};

static int verify_vector_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct vector_check *check = opaque;
    struct managed_state *state = check->state;
    unsigned vector = check->vector;
    (void)lifecycle; (void)view;
    struct mem_service_visibility_completion completion;
    float expected[VECTOR_ELEMENTS];
    for (unsigned i = 0; i < VECTOR_ELEMENTS; ++i) {
        if (check->negative_operation && vector >= 2) {
            const uint32_t sentinel = 0x7fc00001;
            memcpy(&expected[i], &sentinel, sizeof(sentinel));
        } else {
            expected[i] = expected_value(vector, i);
        }
    }
    uint64_t checksum = mem_service_provider_checksum64(expected, sizeof(expected));
    int rc = mem_service_provider_channel_wait_range_visible(channel,
        &mapping->binding, vector_offset(vector), VECTOR_BYTES, checksum,
        state->config.timeout_ms, &completion);
    if (rc) return rc;
    const void *actual = (const uint8_t *)mapping->base + vector_offset(vector);
    if (memcmp(actual, expected, sizeof(expected))) return -EIO;
    if (check->negative_operation) {
        printf("MANAGED_PTO_NEGATIVE_DATA node=%u case=%s op_id=%" PRIu64
            " vector=%u checksum=%016" PRIx64 " bytes=%zu unchanged=pass\n",
            state->config.node, check->path, check->negative_operation, vector,
            checksum, sizeof(expected));
    } else printf("MANAGED_PTO node=%u path=%s vector=%u version=%u checksum=%016" PRIx64
           " bytes=%zu cpu_verify=pass\n", state->config.node, check->path, vector,
           vector < 2 ? 1 : vector, checksum, sizeof(expected));
    return 0;
}

static int verify_vector(struct managed_state *state, unsigned vector, const char *path)
{
    struct vector_check check = {state, vector, path, 0};
    return mem_service_mapping_lease_apply(state->cpu_pin, verify_vector_callback, &check);
}

static int seed_inputs_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    (void)opaque; (void)lifecycle; (void)view;
    struct mem_service_visibility_completion completion;
    for (unsigned vector = 0; vector < 4; ++vector) {
        uint8_t *base = (uint8_t *)mapping->base + vector_offset(vector);
        for (unsigned i = 0; i < VECTOR_ELEMENTS; ++i) {
            if (vector < 2) {
                float value = expected_value(vector, i);
                memcpy(base + i * sizeof(float), &value, sizeof(value));
            } else {
                const uint32_t sentinel = 0x7fc00001;
                memcpy(base + i * sizeof(float), &sentinel, sizeof(sentinel));
            }
        }
    }
    uint64_t checksum = mem_service_provider_checksum64(mapping->base, mapping->len);
    int rc = mem_service_provider_channel_publish_range(channel,
        &mapping->binding, 0, mapping->len, checksum, &completion);
    if (!rc) printf("MANAGED_PTO node=0 inputs_published=1 version=1 bytes=%u\n",
                    MANAGED_BYTES);
    return rc;
}

/* This deployment selects its provider here; tensor consumers use one SDK. */
static int open_provider(struct managed_state *state)
{
    struct mem_service_provider_obmm_config config = {
        .instance = "managed-pto", .import_region_bytes = 2097152,
        .max_remote_mappings = 2, .required_peer_mappings = 1,
    };
    struct mem_service_provider_registration registration;
    struct mem_service_provider_remote_region local, peers[2];
    struct mem_service_region region;
    uint8_t pattern[256];
    uint64_t checksum;
    int rc = mem_service_provider_registry_init(&state->registry);
    if (!rc) rc = mem_service_provider_obmm_endpoint_open(&state->provider, &config);
    if (!rc) rc = mem_service_provider_obmm_endpoint_registration(&state->provider,
                                                                  &registration);
    if (!rc) rc = mem_service_provider_obmm_endpoint_prepare_canary_region(
        &state->provider, config.import_region_bytes, sizeof(pattern),
        (uint8_t)(41 + state->config.node), &region, &local, &checksum);
    if (!rc) rc = mem_service_provider_obmm_endpoint_exchange_remote_regions(
        &state->provider, state->config.node, 2, state->config.generation,
        &local, peers, 2);
    if (rc) return rc;
    unsigned peer = 1 - state->config.node;
    for (size_t i = 0; i < sizeof(pattern); ++i) pattern[i] = (uint8_t)(41 + peer + i * 29U);
    rc = mem_service_provider_obmm_endpoint_verify_mapping(&state->provider,
        &peers[peer], 0, sizeof(pattern),
        mem_service_provider_checksum64(pattern, sizeof(pattern)), state->config.timeout_ms);
    if (!rc) rc = mem_service_provider_registry_register(&state->registry, &registration);
    if (!rc) rc = mem_service_provider_channel_bind(&state->registry, registration.name,
        registration.instance, MEM_SERVICE_PROVIDER_CAP_PEER_MAPPING, &state->channel);
    if (!rc) printf("MANAGED_PTO node=%u peer_canary=pass\n", state->config.node);
    return rc;
}

static int wait_allocation(struct managed_state *state, const char *wanted,
                           bool consumer_drained)
{
    uint64_t started = now_ms();
    do {
        enum mem_service_wire_status status;
        int rc = mem_service_client_inspect_allocation(&state->client, state->config.key,
                                                       &state->allocation, &status);
        if (!rc && !strcmp(state->allocation.state, wanted) &&
            (!consumer_drained || (state->allocation.live_refs == 1 &&
             state->allocation.holder_count == 1 &&
             !strcmp(state->allocation.holders[0].session_id, state->session)))) return 0;
        if (rc && status != MEM_SERVICE_WIRE_STATUS_NOT_FOUND) return rc;
        if (!rc && !strcmp(state->allocation.state, "quarantined")) return -ESTALE;
        usleep(10000);
    } while (now_ms() - started < state->config.timeout_ms);
    return -ETIMEDOUT;
}

static int map_object(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char idem[80], operation[80];
    int rc;
    if (!state->config.node) {
        snprintf(idem, sizeof(idem), "pto-alloc-%" PRIu64, state->config.generation);
        struct mem_service_client_allocate request = {
            .key = state->config.key, .idempotency_key = idem,
            .session_id = state->session, .size_bytes = MANAGED_BYTES,
            .capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP,
        };
        rc = mem_service_client_allocate_object(&state->client, &request,
                                                &state->allocation, &status);
        if (rc) return rc;
    }
    rc = wait_allocation(state, "active", false);
    if (rc) return rc;
    if (state->allocation.size_bytes != MANAGED_BYTES || !state->allocation.provider_backed)
        return -ESTALE;
    snprintf(idem, sizeof(idem), "pto-acquire-%s", state->session);
    rc = mem_service_client_acquire_object(&state->client, state->config.key, idem,
        state->session, true, state->allocation.generation, &state->allocation, &status);
    if (rc) return rc;
    state->held = true;
    uint8_t nonce[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t count = read(fd, nonce, sizeof(nonce));
    close(fd);
    if (count != sizeof(nonce)) return -EIO;
    memcpy(operation, "pto-map-", 8);
    for (size_t i = 0; i < sizeof(nonce); ++i)
        snprintf(operation + 8 + 2 * i, 3, "%02x", nonce[i]);
    rc = mem_service_client_map_managed_allocation(&state->client, &state->channel,
        &state->allocation, state->session, operation,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &state->mapping, &state->lifecycle, &status);
    if (!rc) printf("MANAGED_PTO node=%u key=%s generation=%" PRIu64
        " cpu_base=0x%" PRIx64 " address=0x%" PRIx64 " logical_bytes=%" PRIu64
        " mapping_id=%" PRIu64 " mapping_handle=%" PRIu64 "\n",
        state->config.node, state->mapping.key, state->mapping.generation,
        (uint64_t)(uintptr_t)state->mapping.base, state->allocation.address,
        state->mapping.len, state->lifecycle.transaction.mapping_id,
        state->mapping.binding.mapping.handle);
    return rc;
}

static int release_compute(struct managed_state *state)
{
    if (state->completion_unknown) return -EBUSY;
    if (state->inflight) {
        lingqu_shmem_pto_dispatch_finish(state->inflight);
        state->inflight = NULL;
    }
    for (unsigned i = 0; i < 3; ++i) {
        if (!state->leases[i]) continue;
        int rc = lingqu_shmem_mem_service_release(state->leases[i]);
        if (rc) return rc;
        state->leases[i] = NULL;
    }
    return 0;
}

static int adopt_mapping(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char release_operation[80];
    snprintf(release_operation, sizeof(release_operation), "pto-release-%s", state->session);
    state->mapping_id = state->lifecycle.transaction.mapping_id;
    int rc = mem_service_access_domain_create(&state->client, &state->channel, &state->domain);
    if (!rc) rc = mem_service_mapping_owner_adopt(state->domain, &state->mapping,
        &state->lifecycle, release_operation, MAX_ACCESS_THREADS + 4, &state->owner, &status);
    if (rc) return rc;
    state->held = false; /* This exact holder now belongs to the SDK owner. */
    struct mem_service_mapping_access_view view;
    rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        0, MANAGED_BYTES, MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &state->cpu_pin, &view);
    if (!rc) printf("MANAGED_OWNER node=%u state=adopted generation=%" PRIu64
        " mapping_id=%" PRIu64 " cpu_pins=1 holder_transferred=1\n",
        state->config.node, state->allocation.generation, state->mapping_id);
    return rc;
}

static void *cpu_worker(void *opaque)
{
    struct managed_state *state = opaque;
    struct mem_service_mapping_lease *pin = NULL;
    struct mem_service_mapping_access_view view;
    int rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        vector_offset(0), VECTOR_BYTES, MEM_SERVICE_CLIENT_MAP_READ, &pin, &view);
    bool ready = false, observed_seal = false;
    do {
        pthread_mutex_lock(&state->workers_lock);
        bool stop = state->workers_stop, sealed = state->owner_sealed;
        pthread_mutex_unlock(&state->workers_lock);
        if (stop) break;
        if (!rc) {
            const volatile float *actual = view.base;
            for (unsigned i = 0; i < VECTOR_ELEMENTS; ++i)
                if (actual[i] != expected_value(0, i)) { rc = -EIO; break; }
        }
        pthread_mutex_lock(&state->workers_lock);
        if (!ready) { ++state->workers_ready; ready = true; }
        if (rc) state->worker_error = rc;
        else {
            ++state->worker_checks;
            if (sealed && !observed_seal) {
                ++state->workers_observed_seal;
                observed_seal = true;
            }
        }
        pthread_cond_broadcast(&state->workers_condition);
        pthread_mutex_unlock(&state->workers_lock);
        if (!rc) usleep(1000);
    } while (!rc);
    if (pin) {
        int release_rc = mem_service_mapping_lease_release(pin);
        if (release_rc) {
            pthread_mutex_lock(&state->workers_lock);
            state->worker_error = release_rc;
            pthread_cond_broadcast(&state->workers_condition);
            pthread_mutex_unlock(&state->workers_lock);
        }
    }
    return NULL;
}

static int start_workers(struct managed_state *state)
{
    for (unsigned i = 0; i < state->config.access_threads; ++i) {
        int rc = pthread_create(&state->workers[i], NULL, cpu_worker, state);
        if (rc) return -rc;
        ++state->workers_started;
    }
    pthread_mutex_lock(&state->workers_lock);
    while (state->workers_ready < state->workers_started)
        pthread_cond_wait(&state->workers_condition, &state->workers_lock);
    int rc = state->worker_error;
    pthread_mutex_unlock(&state->workers_lock);
    return rc;
}

static int probe_owner_close(struct managed_state *state, bool in_flight)
{
    struct mem_service_mapping_owner_stats stats;
    enum mem_service_wire_status status;
    if (mem_service_mapping_owner_close(state->owner, &status) != -EBUSY) return -EPROTO;
    int rc = mem_service_mapping_owner_inspect(state->owner, &stats);
    if (rc) return rc;
    unsigned expected_compute = in_flight ? 3 : 0;
    if (!stats.closing || !stats.mapping_pending || !stats.holder_pending ||
        stats.cpu_pins != 1 + state->workers_started ||
        stats.compute_pins != expected_compute || stats.in_flight_pins != expected_compute)
        return -EPROTO;
    struct mem_service_mapping_lease *blocked = NULL;
    struct mem_service_mapping_access_view view;
    rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        vector_offset(0), VECTOR_BYTES, MEM_SERVICE_CLIENT_MAP_READ, &blocked, &view);
    if (rc != -ECANCELED || blocked) return -EPROTO;
    pthread_mutex_lock(&state->workers_lock);
    state->owner_sealed = true;
    while (state->workers_observed_seal < state->workers_started && !state->worker_error)
        pthread_cond_wait(&state->workers_condition, &state->workers_lock);
    rc = state->worker_error;
    pthread_mutex_unlock(&state->workers_lock);
    if (!rc) printf("MANAGED_OWNER node=1 state=closing cpu_pins=%u compute_pins=%u "
        "in_flight_pins=%u close_guard=pass admission_guard=pass threads=%u operation_id=%" PRIu64 "\n",
        stats.cpu_pins, stats.compute_pins, stats.in_flight_pins, state->workers_started,
        in_flight ? state->config.generation + 2 : 0);
    return rc;
}

static int stop_workers(struct managed_state *state)
{
    pthread_mutex_lock(&state->workers_lock);
    state->workers_stop = true;
    pthread_mutex_unlock(&state->workers_lock);
    for (unsigned i = 0; i < state->workers_started; ++i) {
        int rc = pthread_join(state->workers[i], NULL);
        if (rc) return -rc;
    }
    if (state->worker_error) return state->worker_error;
    printf("MANAGED_OWNER node=1 state=workers-drained threads=%u checks=%u "
        "after_seal_threads=%u cpu_verify=pass\n", state->workers_started,
        state->worker_checks, state->workers_observed_seal);
    state->workers_started = 0;
    return 0;
}

static int close_mapping(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char idem[80];
    int rc = release_compute(state);
    if (rc) return rc;
    if (state->workers_started) return -EBUSY;
    if (state->cpu_pin) {
        rc = mem_service_mapping_lease_release(state->cpu_pin);
        if (rc) return rc;
        state->cpu_pin = NULL;
    }
    if (state->owner) {
        rc = mem_service_mapping_owner_close(state->owner, &status);
        if (rc) return rc;
        struct mem_service_mapping_owner_stats stats;
        rc = mem_service_mapping_owner_inspect(state->owner, &stats);
        if (rc || stats.cpu_pins || stats.compute_pins || stats.in_flight_pins ||
            stats.mapping_pending || stats.holder_pending) return -EPROTO;
        rc = mem_service_mapping_owner_destroy(state->owner);
        if (rc) return rc;
        state->owner = NULL;
        printf("MANAGED_OWNER node=%u state=drained cpu_pins=0 compute_pins=0 "
               "in_flight_pins=0 mapping_pending=0 holder_pending=0\n", state->config.node);
    }
    if (state->domain) {
        rc = mem_service_access_domain_destroy(state->domain);
        if (rc) return rc;
        state->domain = NULL;
    }
    if (state->lifecycle.pending || state->mapping.binding.mapped) {
        rc = mem_service_client_unmap_managed_allocation(&state->client, &state->channel,
            &state->mapping, &state->lifecycle, &status);
        if (rc) return rc;
    }
    if (state->held) {
        snprintf(idem, sizeof(idem), "pto-release-%s", state->session);
        rc = mem_service_client_release_object(&state->client, state->config.key, idem,
            state->session, true, state->allocation.generation, &state->allocation, &status);
        if (rc) return rc;
        state->held = false;
    }
    return 0;
}

static int inject_negative(struct managed_state *state, unsigned fault,
                           uint64_t metadata_iova, struct lingqu_shmem_pto_wire_result *wire)
{
    LingquPtoDispatchControlV2 *control = state->metadata;
    if (!fault || fault > NEGATIVE_CASES || control->memref_count != 3 ||
        control->scalar_count || control->memref_table_iova < metadata_iova)
        return -EINVAL;
    uint64_t offset = control->memref_table_iova - metadata_iova;
    if (offset > METADATA_BYTES || 3 * sizeof(LingquShmemMemrefV1) > METADATA_BYTES - offset)
        return -EPROTO;
    LingquShmemMemrefV1 *refs = (void *)((uint8_t *)state->metadata + offset);
    switch (fault) {
    case 1: {
        uint64_t original = refs[0].opaque_mapping_ref;
        uint64_t generation = lingqu_pto_obmm_mapping_ref_generation(original);
        uint64_t changed = generation == LINGQU_PTO_OBMM_MAP_GENERATION_MAX ?
                           generation - 1 : generation + 1;
        refs[0].opaque_mapping_ref = lingqu_pto_obmm_mapping_ref_encode(
            lingqu_pto_obmm_mapping_ref_map_id(original), changed);
        if (!refs[0].opaque_mapping_ref || refs[0].opaque_mapping_ref == original)
            return -ERANGE;
        break;
    }
    case 2: control->requester_cna = state->config.requester == 1 ? 2 : 1; break;
    case 3: refs[2].byte_offset = MANAGED_BYTES - VECTOR_BYTES + 1; break;
    case 4: refs[2].access = LINGQU_PTO_UB_GM_READ; break;
    default: return -EINVAL;
    }
    return lingqu_shmem_pto_refresh_fault_metadata_crc(state->metadata,
        METADATA_BYTES, metadata_iova, wire);
}

static int dispatch_vector_checked(struct managed_state *state, unsigned round, unsigned fault)
{
    struct lingqu_shmem_memref *memrefs[3] = {0};
    struct lingqu_shmem_pto_memref_arg args[3];
    struct lingqu_shmem_pto_wire_result wire;
    struct lingqu_shmem_pto_completion completion;
    LingquPtoDispatchSlotV2 slot;
    uint64_t metadata_iova;
    unsigned vectors[3] = {round == 1 ? 0 : 2, 1, round + 1};
    int rc;
    for (unsigned i = 0; i < 3; ++i) {
        struct lingqu_shmem_memref_spec spec = {
            .byte_offset = vector_offset(vectors[i]), .byte_length = VECTOR_BYTES,
            .rank = 2, .dtype = 0, .shape = {1, VECTOR_ELEMENTS},
            .strides = {VECTOR_ELEMENTS, 1},
            .access = i == 2 ? LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ,
        };
        rc = lingqu_shmem_mem_service_acquire_owned(state->compute, state->owner,
            &spec, &memrefs[i], &state->leases[i]);
        if (rc) return rc;
        args[i] = (struct lingqu_shmem_pto_memref_arg) {.memref = memrefs[i], .arg_index = i};
    }
    rc = lingqu_shmem_sim_phys_for_virt(state->metadata, &metadata_iova);
    if (rc) return rc;
    struct lingqu_shmem_pto_request request = {
        .op_id = state->config.generation + (fault ? 2 + fault : round),
        .request_id = state->config.generation + (fault ? 2 + fault : round), .callable_id = 1,
        .artifact_fingerprint = state->config.fingerprint,
        .requester_cna = state->config.requester, .memrefs = args, .memref_count = 3,
    };
    rc = lingqu_shmem_pto_dispatch_prepare(&request, state->metadata, METADATA_BYTES,
        metadata_iova, &slot, &wire, &state->inflight);
    if (rc) return rc;
    if (fault) {
        rc = inject_negative(state, fault, metadata_iova, &wire);
        if (rc) return rc;
    }
    /* The public lifetime guard must retain every prepared memref. */
    for (unsigned i = 0; i < 3; ++i) {
        rc = lingqu_shmem_mem_service_release(state->leases[i]);
        if (rc != -EBUSY) {
            if (!rc) state->leases[i] = NULL;
            return -EPROTO;
        }
    }
    if (lingqu_shmem_mem_service_active_leases(state->compute) != 3) return -EPROTO;
    for (unsigned i = 0; i < 3; ++i) {
        rc = lingqu_shmem_mem_service_lease_begin(state->leases[i], request.op_id);
        if (rc) return rc;
    }
    const char *expected_error = fault == 2 ? LINGQU_PTO_UB_GM_CODE_ACCESS_DENIED :
                                            LINGQU_PTO_UB_GM_CODE_BAD_MEMREF;
    if (fault) {
        printf("MANAGED_PTO_NEGATIVE node=1 case=%s state=submitted op_id=%" PRIu64
            " mapping_id=%" PRIu64 " expected=%s metadata_crc=0x%08x retained_leases=3\n",
            negative_names[fault - 1], request.op_id, state->mapping_id,
            expected_error, wire.metadata_crc32);
    } else printf("MANAGED_PTO node=1 dispatch=%u op_id=%" PRIu64 " generation=%" PRIu64
        " mapping_id=%" PRIu64 " offsets=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " tensor_bytes=%zu retained_leases=3 release_guard=pass\n",
        round, request.op_id, state->allocation.generation,
        state->mapping_id, vector_offset(vectors[0]),
        vector_offset(vectors[1]), vector_offset(vectors[2]), (size_t)VECTOR_BYTES);
    state->completion_unknown = true;
    rc = lingqu_shmem_pto_endpoint_submit(state->endpoint, &slot,
                                          state->config.submit_timeout_ms, &completion);
    bool recovered = false;
    if (rc == -ETIMEDOUT && state->config.recovery_expected) {
        if (lingqu_shmem_pto_endpoint_pending_op(state->endpoint) != request.op_id ||
            lingqu_shmem_pto_endpoint_close_checked(state->endpoint) != -EBUSY ||
            release_compute(state) != -EBUSY) return -EPROTO;
        for (unsigned i = 0; i < 3; ++i) {
            rc = lingqu_shmem_mem_service_release(state->leases[i]);
            if (rc != -EBUSY) {
                if (!rc) state->leases[i] = NULL;
                return -EPROTO;
            }
        }
        if (lingqu_shmem_mem_service_active_leases(state->compute) != 3) return -EPROTO;
        if (round == 2) {
            rc = probe_owner_close(state, true);
            if (rc) return rc;
        }
        printf("MANAGED_PTO_WAIT node=1 dispatch=%u op_id=%" PRIu64
            " state=timeout retained_leases=3 close_guard=pass release_guard=pass\n",
            round, request.op_id);
        rc = lingqu_shmem_pto_endpoint_wait(state->endpoint, request.op_id,
                                            state->config.timeout_ms, &completion);
        if (!rc) {
            if (lingqu_shmem_pto_endpoint_pending_op(state->endpoint)) return -EPROTO;
            recovered = true;
            printf("MANAGED_PTO_WAIT node=1 dispatch=%u op_id=%" PRIu64
                " state=recovered completion_op=%" PRIu64 " pending_op=0\n",
                round, request.op_id, completion.op_id);
        }
    }
    if (rc) return rc;
    if (completion.op_id != request.op_id) return -EPROTO;
    for (unsigned i = 0; i < 3; ++i) {
        rc = lingqu_shmem_mem_service_lease_complete(state->leases[i], request.op_id);
        if (rc) return rc;
    }
    state->completion_unknown = false;
    if (fault) {
        if (completion.status != 3 || strcmp(completion.error_code, expected_error) ||
            lingqu_shmem_pto_endpoint_pending_op(state->endpoint)) return -EPROTO;
        printf("MANAGED_PTO_NEGATIVE node=1 case=%s state=rejected op_id=%" PRIu64
            " completion_op=%" PRIu64 " status=%u error=%s pending_op=0\n",
            negative_names[fault - 1], request.op_id, completion.op_id,
            completion.status, completion.error_code);
        rc = release_compute(state);
        for (unsigned vector = 0; !rc && vector < 4; ++vector) {
            struct vector_check check = {state, vector, negative_names[fault - 1], request.op_id};
            rc = mem_service_mapping_lease_apply(state->cpu_pin, verify_vector_callback, &check);
        }
        return rc;
    }
    if (state->config.recovery_expected && !recovered) return -EPROTO;
    printf("MANAGED_PTO node=1 dispatch=%u completion_op=%" PRIu64
        " status=%u error=%s\n", round, completion.op_id, completion.status,
        completion.error_code[0] ? completion.error_code : "none");
    if (!lingqu_shmem_pto_completion_succeeded(&completion)) return -EIO;
    rc = release_compute(state);
    if (rc) return rc;
    return verify_vector(state, vectors[2], round == 1 ? "tstore-cpu" : "tstore-tload-cpu");
}

static int dispatch_vector(struct managed_state *state, unsigned round)
{
    return dispatch_vector_checked(state, round, 0);
}

static int run_data(struct managed_state *state)
{
    int rc = map_object(state);
    if (!rc) rc = adopt_mapping(state);
    if (rc) return rc;
    if (!state->config.node) {
        rc = mem_service_mapping_lease_apply(state->cpu_pin, seed_inputs_callback, NULL);
        if (!rc) rc = verify_vector(state, 2, "remote-tstore-cpu");
        if (!rc) rc = verify_vector(state, 3, "remote-tstore-tload-cpu");
        if (!rc) rc = wait_allocation(state, "active", true);
        return rc;
    }
    rc = verify_vector(state, 0, "cpu-cpu");
    if (!rc) rc = verify_vector(state, 1, "cpu-cpu");
    if (!rc) rc = lingqu_shmem_mem_service_open_managed(&state->compute);
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    if (!rc) rc = lingqu_shmem_pto_endpoint_open(&state->endpoint, &endpoint_info);
    if (rc) return rc;
    state->metadata = mmap(NULL, METADATA_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (state->metadata == MAP_FAILED) { state->metadata = NULL; return -errno; }
    memset(state->metadata, 0, METADATA_BYTES);
    if (mlock(state->metadata, METADATA_BYTES)) return -errno;
    rc = start_workers(state);
    for (unsigned fault = 1; !rc && state->config.negative_checks && fault <= NEGATIVE_CASES; ++fault)
        rc = dispatch_vector_checked(state, 1, fault);
    if (!rc) rc = dispatch_vector(state, 1);
    if (!rc) rc = dispatch_vector(state, 2);
    if (!rc && !state->owner_sealed) rc = probe_owner_close(state, false);
    if (!rc) rc = stop_workers(state);
    return rc;
}

int lingqu_shmem_pto_managed_main(const char *config_path, bool validate_only)
{
    struct managed_config config;
    struct managed_state *state;
    const char *stage = "config";
    int rc = read_config(config_path, &config);
    if (rc) {
        fprintf(stderr, "MANAGED_PTO_RESULT status=fail stage=config rc=%d\n", rc);
        return 2;
    }
    if (validate_only) {
        printf("MANAGED_PTO_CONFIG status=ok node=%u nodes=2 bytes=%u layout=cross-page "
            "elements=%u callable=1 network_operations=0 submit_timeout_ms=%" PRIu64
            " recovery_expected=%u access_threads=%u negative_checks=%u\n",
            config.node, MANAGED_BYTES, VECTOR_ELEMENTS, config.submit_timeout_ms,
            config.recovery_expected ? 1 : 0, config.access_threads, config.negative_checks ? 1 : 0);
        return 0;
    }
    state = calloc(1, sizeof(*state));
    if (!state) return 1;
    rc = pthread_mutex_init(&state->workers_lock, NULL);
    if (rc) { free(state); return 1; }
    rc = pthread_cond_init(&state->workers_condition, NULL);
    if (rc) { pthread_mutex_destroy(&state->workers_lock); free(state); return 1; }
    state->config = config;
    snprintf(state->session, sizeof(state->session), "managed-pto-%u-%" PRIu64,
              config.node, config.generation);
    state->client.connect_spec = state->config.connect;
    mem_service_wire_client_options_init(&state->client.wire_options);
    state->client.wire_options.timeout_ms = config.timeout_ms;
    setvbuf(stdout, NULL, _IOLBF, 0);
    stage = "provider";
    rc = open_provider(state);
    if (rc) goto failed;
    stage = "data";
    rc = run_data(state);
    if (rc) goto failed;
    stage = "unmap-release";
    rc = close_mapping(state);
    if (rc) goto failed;
    if (!config.node) {
        enum mem_service_wire_status status;
        char idem[80];
        stage = "retire";
        snprintf(idem, sizeof(idem), "pto-retire-%" PRIu64, config.generation);
        rc = mem_service_client_retire_object(&state->client, config.key, idem, true,
            state->allocation.generation, &state->allocation, &status);
        if (!rc) rc = wait_allocation(state, "retired", false);
        if (rc) goto failed;
    }
    stage = "compute-close";
    if (state->compute) {
        rc = lingqu_shmem_mem_service_close(state->compute);
        if (rc) goto failed;
        state->compute = NULL;
    }
    rc = lingqu_shmem_pto_endpoint_close_checked(state->endpoint);
    if (rc) goto failed;
    state->endpoint = NULL;
    if (state->metadata) {
        if (munmap(state->metadata, METADATA_BYTES)) { rc = -errno; goto failed; }
        state->metadata = NULL;
    }
    stage = "provider-close";
    rc = mem_service_provider_obmm_endpoint_close_checked(&state->provider);
    if (rc) goto failed;
    printf("MANAGED_PTO_RESULT node=%u status=pass key=%s generation=%" PRIu64
        " mapping_closed=1 holder_released=1 compute_leases=0\n",
        config.node, config.key, state->allocation.generation);
    pthread_cond_destroy(&state->workers_condition);
    pthread_mutex_destroy(&state->workers_lock);
    free(state);
    return 0;
failed:
    /* The headless runner terminates the entire guest on this marker. Keeping
     * this process alive retains its mappings, metadata and holder meanwhile;
     * a timeout alone cannot prove that the device stopped accessing them. */
    fprintf(stderr, "MANAGED_PTO_RESULT node=%u status=fail stage=%s rc=%d "
        "completion_unknown=%u resources_retained=1\n", config.node, stage, rc,
        state->completion_unknown ? 1 : 0);
    fflush(NULL);
    for (;;) pause();
}
