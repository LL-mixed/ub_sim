/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_pto_managed.h"
#include "lingqu_shmem_pto_cpu_probe.h"
#include "lingqu_shmem_mem_service.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "lingqu_shmem_sim.h"
#include "w5_managed_model_guest.h"
#include "components/llm_infer/llm_infer.h"
#include "components/mem_service/mem_service_client.h"
#include "components/mem_service/mem_service_mapping_owner.h"
#include "components/mem_service/mem_service_object_contract.h"
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

#define MAX_VECTOR_ELEMENTS 16384U
#define METADATA_BYTES 4096U
#define MAX_ACCESS_THREADS 8U
#define NEGATIVE_CASES 6U
#define MODEL_HIDDEN_BYTES 2048U
#define MODEL_OUTPUT_OFFSET 16384U

static const char *const negative_names[NEGATIVE_CASES] = {
    "bad-mapping-ref", "wrong-requester", "oob", "role-access-mismatch",
    "tstore-on-read", "tload-on-write"
};

struct managed_geometry {
    const char *name;
    uint32_t rows, columns, elements, tensor_bytes, object_bytes;
};

static const struct managed_geometry managed_geometries[] = {
    {"cross-page", 1, 64, 64, 256, 65536},
    {"nd", 128, 128, 16384, 65536, 262144},
    {"tail", 128, 127, 16256, 65024, 262144},
};

struct managed_config {
    char connect[128], key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
    uint64_t generation, fingerprint, timeout_ms, submit_timeout_ms;
    uint32_t node, home_node, requester, access_threads, concurrent_requests;
    bool recovery_expected;
    bool negative_checks;
    bool readonly_inputs;
    bool reference_inputs;
    bool model_probe;
    bool model_publish;
    bool holder_recovery_probe;
    uint64_t model_output_bytes, model_output_checksum;
    struct managed_geometry geometry;
};

struct managed_batch;

struct managed_state {
    struct managed_config config;
    struct mem_service_client client;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_obmm_endpoint provider;
    struct mem_service_client_allocation allocation;
    struct mem_service_client_object_mapping mapping;
    struct mem_service_client_mapping_lifecycle lifecycle;
    struct mem_service_client_reference_lifecycle reference_lifecycle;
    struct mem_service_reference_request reference_request;
    struct lingqu_object_ref_wire_v2 reference;
    struct lingqu_object_ref_wire_v2 published_outputs[3];
    struct llm_infer_range_result decoded_output;
    unsigned output_view;
    uint64_t view_offset;
    struct mem_service_access_domain *domain;
    struct mem_service_mapping_owner *owner;
    struct mem_service_mapping_lease *cpu_pin;
    struct managed_state *output;
    bool is_output, owns_domain;
    uint64_t mapping_id, readonly_checksum;
    struct lingqu_shmem_mem_service_context *compute;
    struct lingqu_shmem_mem_service_lease *leases[3];
    struct lingqu_shmem_pto_endpoint *endpoint;
    struct lingqu_shmem_pto_inflight *inflight;
    struct w5_managed_model_operation *model_operation;
    void *metadata;
    uint64_t metadata_iova;
    struct managed_batch *batch;
    unsigned request_round;
    char session[MEM_SERVICE_CLIENT_ALLOCATION_SESSION_ID_LEN];
    bool held, completion_unknown;
    pthread_mutex_t workers_lock;
    pthread_cond_t workers_condition;
    pthread_t workers[MAX_ACCESS_THREADS];
    unsigned workers_started, workers_ready, worker_checks, workers_observed_seal;
    bool workers_stop, owner_sealed;
    int worker_error, holder_recovery_cpu_signal;
};

struct managed_batch {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    struct managed_state *requests[2];
    pthread_t threads[2];
    unsigned started, submitted;
    bool allow_poll;
    int error;
};

static uint64_t now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* OBMM bootstrap generation is a shared discovery namespace. Keep it
 * independent from the provider incarnation used by holder fencing: the
 * provider worker and this PTO endpoint are separate publishers. */
static uint64_t managed_provider_bootstrap_generation(
    const struct managed_config *config)
{
    static const char domain[] = "mem-service-managed-pto-provider-v1";
    const uint64_t prime = UINT64_C(1099511628211);
    uint64_t hash = UINT64_C(1469598103934665603);
    uint64_t words[2] = {config->generation, config->fingerprint};
    for (size_t i = 0; i < sizeof(domain) - 1; ++i) {
        hash ^= (uint8_t)domain[i];
        hash *= prime;
    }
    for (size_t i = 0; config->key[i] != '\0'; ++i) {
        hash ^= (uint8_t)config->key[i];
        hash *= prime;
    }
    for (size_t word = 0; word < 2; ++word) {
        for (unsigned byte = 0; byte < 8; ++byte) {
            hash ^= (uint8_t)(words[word] >> (byte * 8));
            hash *= prime;
        }
    }
    if (hash == 0 || hash == config->generation) {
        hash = config->generation ^ UINT64_C(0xd6e8feb86659fd93);
    }
    if (hash == 0 || hash == config->generation) {
        hash = config->generation == UINT64_MAX ? 1 : config->generation + 1;
    }
    return hash;
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
        "access_threads", "negative_checks", "readonly_inputs", "layout", "elements", "size_bytes",
        "home_node", "concurrent_requests", "reference_inputs", "model_probe",
        "model_output_bytes", "model_output_checksum", "model_publish",
        "holder_recovery_probe"};
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    unsigned seen = 0;
    uint64_t expected_elements = 0, expected_bytes = 0;
    FILE *file = fopen(path, "r");
    int rc = -EINVAL;
    if (!file) return -errno;
    memset(config, 0, sizeof(*config));
    config->geometry = managed_geometries[0];
    config->concurrent_requests = 1;
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
        if (field == 11) {
            size_t index;
            for (index = 0; index < sizeof(managed_geometries) / sizeof(managed_geometries[0]);
                 ++index)
                if (!strcmp(value, managed_geometries[index].name)) break;
            if (index == sizeof(managed_geometries) / sizeof(managed_geometries[0])) goto out;
            config->geometry = managed_geometries[index];
        } else if (field < 2) {
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
            case 10: if (number > 1) goto out;
                     config->readonly_inputs = number != 0; break;
            case 12: expected_elements = number; break;
            case 13: expected_bytes = number; break;
            case 14: if (number > 1) goto out;
                     config->home_node = (uint32_t)number; break;
            case 15: if (number < 1 || number > 2) goto out;
                     config->concurrent_requests = (uint32_t)number; break;
            case 16: if (number > 1) goto out;
                     config->reference_inputs = number != 0; break;
            case 17: if (number > 1) goto out;
                     config->model_probe = number != 0; break;
            case 18: config->model_output_bytes = number; break;
            case 19: config->model_output_checksum = number; break;
            case 20: if (number > 1) goto out;
                     config->model_publish = number != 0; break;
            case 21: if (number > 1) goto out;
                     config->holder_recovery_probe = number != 0; break;
            default: goto out;
            }
        }
    }
    if (ferror(file) || (seen & 127) != 127) goto out;
    if (!valid_connect(config->connect)) goto out;
    if (((seen & (1U << 12)) && expected_elements != config->geometry.elements) ||
        ((seen & (1U << 13)) && expected_bytes != config->geometry.object_bytes)) goto out;
    if (config->readonly_inputs && strlen(config->key) + strlen("-output") >=
        sizeof(config->key)) goto out;
    if (config->reference_inputs && !config->readonly_inputs) goto out;
    if (config->model_publish && !config->model_probe) goto out;
    config->recovery_expected = (seen & 128) != 0;
    if (config->model_probe) {
        if (!config->reference_inputs || config->home_node != 1 ||
            strcmp(config->geometry.name, "nd") || config->access_threads ||
            config->concurrent_requests != 1 || config->negative_checks ||
            config->recovery_expected || !config->model_output_bytes ||
            config->model_output_bytes > config->geometry.object_bytes - MODEL_OUTPUT_OFFSET ||
            !(seen & (1U << 19))) goto out;
    } else if (seen & ((1U << 18) | (1U << 19))) goto out;
    if (config->concurrent_requests == 2 &&
        (config->negative_checks || config->recovery_expected)) goto out;
    if (config->negative_checks && (config->recovery_expected ||
        config->generation > UINT64_MAX - 2 - NEGATIVE_CASES)) goto out;
    if (config->recovery_expected) {
        if (config->submit_timeout_ms >= config->timeout_ms) goto out;
    } else {
        config->submit_timeout_ms = config->timeout_ms;
    }
    if (config->holder_recovery_probe &&
        (config->negative_checks || config->readonly_inputs ||
         config->reference_inputs || config->model_probe || config->model_publish ||
         config->recovery_expected || config->access_threads ||
         config->concurrent_requests != 1)) goto out;
    rc = 0;
out:
    free(line);
    fclose(file);
    return rc;
}

static uint64_t vector_offset(const struct managed_state *state, unsigned vector)
{
    const struct managed_geometry *geometry = &state->config.geometry;
    uint64_t offset = geometry->rows == 1 ?
        (uint64_t)(vector + 1) * 4096 - geometry->tensor_bytes / 2 :
        (uint64_t)vector * geometry->tensor_bytes;
    return offset - state->view_offset;
}

static const char *object_label(const struct managed_state *state)
{
    if (!state->config.readonly_inputs) return "shared";
    return state->is_output ? "output" : "input";
}

static uint64_t mapping_access(const struct managed_state *state)
{
    if (state->config.model_publish && state->is_output &&
        state->config.node == state->config.home_node) return MEM_SERVICE_CLIENT_MAP_READ;
    return MEM_SERVICE_CLIENT_MAP_READ |
        ((state->config.readonly_inputs &&
          (state->config.reference_inputs || state->config.node != state->config.home_node) &&
          !state->is_output) ?
         0 : MEM_SERVICE_CLIENT_MAP_WRITE);
}

static struct managed_state *vector_state(struct managed_state *state, unsigned vector)
{
    return state->output && vector >= 2 ? state->output : state;
}

static float expected_value(unsigned vector, unsigned element)
{
    float a = (float)(element % 8 + 1), b = (float)(element % 4 + 1);
    /* Keep the original small oracle; larger views must not repeat it. */
    if (element >= 64) { a = (float)(element / 8 + 1); b = (float)(element % 8 + 1); }
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
    float expected[MAX_VECTOR_ELEMENTS];
    size_t bytes = state->config.geometry.tensor_bytes;
    for (unsigned i = 0; i < state->config.geometry.elements; ++i) {
        if (check->negative_operation && vector >= 2) {
            const uint32_t sentinel = 0x7fc00001;
            memcpy(&expected[i], &sentinel, sizeof(sentinel));
        } else {
            expected[i] = expected_value(vector == 3 && state->config.concurrent_requests == 2 ?
                                         2 : vector, i);
        }
    }
    uint64_t checksum = mem_service_provider_checksum64(expected, bytes);
    int rc = mem_service_provider_channel_wait_range_visible(channel,
        &mapping->binding, vector_offset(state, vector), bytes, checksum,
        state->config.timeout_ms, &completion);
    if (rc) return rc;
    const void *actual = (const uint8_t *)mapping->base + vector_offset(state, vector);
    if (memcmp(actual, expected, bytes)) return -EIO;
    if (check->negative_operation) {
        printf("MANAGED_PTO_NEGATIVE_DATA node=%u case=%s op_id=%" PRIu64
            " vector=%u checksum=%016" PRIx64 " bytes=%zu unchanged=pass object=%s\n",
            state->config.node, check->path, check->negative_operation, vector,
            checksum, bytes, object_label(state));
    } else printf("MANAGED_PTO node=%u path=%s vector=%u version=%u checksum=%016" PRIx64
           " bytes=%zu cpu_verify=pass object=%s\n", state->config.node, check->path, vector,
           vector < 2 ? 1 : vector, checksum, bytes, object_label(state));
    return 0;
}

static int verify_vector(struct managed_state *state, unsigned vector, const char *path)
{
    state = vector_state(state, vector);
    struct vector_check check = {state, vector, path, 0};
    return mem_service_mapping_lease_apply(state->cpu_pin, verify_vector_callback, &check);
}

static int seed_inputs_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct managed_state *state = opaque;
    (void)lifecycle; (void)view;
    struct mem_service_visibility_completion completion;
    if (state->config.model_probe) {
        if (state->is_output) memset(mapping->base, 0xa5, mapping->len);
        else {
            if (mapping->len < MODEL_HIDDEN_BYTES) return -EINVAL;
            memset(mapping->base, 0, mapping->len);
            for (unsigned i = 0; i < MODEL_HIDDEN_BYTES / 2; ++i) {
                int value = (int)(i % 31) - 15;
                unsigned magnitude = (unsigned)(value < 0 ? -value : value);
                unsigned bit = 0;
                while ((1U << (bit + 1)) <= magnitude) ++bit;
                uint16_t half = magnitude ? (uint16_t)(((bit + 10) << 10) |
                    ((magnitude - (1U << bit)) << (10 - bit)) | (value < 0 ? 0x8000 : 0)) : 0;
                ((uint8_t *)mapping->base)[2 * i] = (uint8_t)half;
                ((uint8_t *)mapping->base)[2 * i + 1] = (uint8_t)(half >> 8);
            }
        }
        return mem_service_provider_channel_publish_range(channel, &mapping->binding,
            0, mapping->len, mem_service_provider_checksum64(mapping->base, mapping->len),
            &completion);
    }
    if (state->config.reference_inputs && !state->is_output)
        memset(mapping->base, 0, mapping->len);
    for (unsigned vector = 0; vector < 4; ++vector) {
        uint8_t *base = (uint8_t *)mapping->base + vector_offset(state, vector);
        for (unsigned i = 0; i < state->config.geometry.elements; ++i) {
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
    if (!rc) printf("MANAGED_PTO node=%u inputs_published=1 version=1 bytes=%u object=%s\n",
                    state->config.node, state->config.geometry.object_bytes, object_label(state));
    return rc;
}

/* This deployment selects its provider here; tensor consumers use one SDK. */
static int open_provider(struct managed_state *state)
{
    struct mem_service_provider_obmm_config config = {
        .instance = "managed-pto", .import_region_bytes = 2097152,
        .max_remote_mappings = state->config.readonly_inputs ? 3 : 2,
        .required_peer_mappings = 1,
    };
    struct mem_service_provider_registration registration;
    struct mem_service_provider_remote_region local, peers[2];
    struct mem_service_region region;
    uint8_t pattern[256];
    uint64_t checksum;
    uint64_t bootstrap_generation =
        managed_provider_bootstrap_generation(&state->config);
    int rc = mem_service_provider_registry_init(&state->registry);
    if (!rc) rc = mem_service_provider_obmm_endpoint_open(&state->provider, &config);
    if (!rc) rc = mem_service_provider_obmm_endpoint_registration(&state->provider,
                                                                  &registration);
    if (!rc) rc = mem_service_provider_obmm_endpoint_prepare_canary_region(
        &state->provider, config.import_region_bytes, sizeof(pattern),
        (uint8_t)(41 + state->config.node), &region, &local, &checksum);
    if (!rc) rc = mem_service_provider_obmm_endpoint_exchange_remote_regions(
        &state->provider, state->config.node, 2, bootstrap_generation,
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
    if (!rc) printf("MANAGED_PTO node=%u peer_canary=pass bootstrap_generation=%" PRIu64
                    "\n", state->config.node, bootstrap_generation);
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

static int wait_holder_recovery_retired(struct managed_state *state)
{
    const uint64_t generation = state->allocation.generation;
    uint64_t started = now_ms();
    do {
        enum mem_service_wire_status status;
        int rc = mem_service_client_inspect_allocation(&state->client,
            state->config.key, &state->allocation, &status);
        if (rc) return rc;
        if (state->allocation.generation != generation) return -ESTALE;
        if (!strcmp(state->allocation.state, "retired")) return 0;
        if (strcmp(state->allocation.state, "active") &&
            strcmp(state->allocation.state, "quarantined") &&
            strcmp(state->allocation.state, "retiring")) return -EPROTO;
        usleep(10000);
    } while (now_ms() - started < state->config.timeout_ms);
    return -ETIMEDOUT;
}

static int mapping_operation(char operation[80])
{
    uint8_t nonce[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    ssize_t count = read(fd, nonce, sizeof(nonce));
    close(fd);
    if (count != sizeof(nonce)) return -EIO;
    memcpy(operation, "pto-map-", 8);
    for (size_t i = 0; i < sizeof(nonce); ++i)
        snprintf(operation + 8 + 2 * i, 3, "%02x", nonce[i]);
    return 0;
}

/* Retain the exact last mutation if its reply is uncertain. The failure path
 * keeps the process and its resources alive; it does not issue a new request. */
static int reference_transition(struct managed_state *state, uint64_t action)
{
    struct mem_service_reference_request *request = &state->reference_request;
    struct mem_service_client_reference_result result;
    enum mem_service_wire_status status;
    memset(request, 0, sizeof(*request));
    request->action = action;
    snprintf(request->key, sizeof(request->key), "%s", state->config.key);
    if (action == MEM_SERVICE_REFERENCE_STAGE || action == MEM_SERVICE_REFERENCE_RESOLVE) {
        const char *suffix = "/input";
        if (state->is_output && state->config.model_publish) {
            const char *suffixes[] = {"/result", "/hidden", "/kv"};
            if (state->output_view >= 3) return -EINVAL;
            suffix = suffixes[state->output_view];
        }
        size_t length = strlen(request->key);
        if (length + strlen(suffix) >= sizeof(request->key)) return -ENAMETOOLONG;
        memcpy(request->key + length, suffix, strlen(suffix) + 1);
    }
    if (action != MEM_SERVICE_REFERENCE_RESOLVE) {
        snprintf(request->session_id, sizeof(request->session_id), "%s", state->session);
        snprintf(request->idempotency_key, sizeof(request->idempotency_key),
                 "pto-ref-%" PRIu64 "-%s-%u", action, state->session, state->output_view);
    }
    if (action == MEM_SERVICE_REFERENCE_BEGIN || action == MEM_SERVICE_REFERENCE_SEAL) {
        request->generation = state->allocation.generation;
        request->version = state->allocation.version;
    }
    if (action == MEM_SERVICE_REFERENCE_STAGE || action == MEM_SERVICE_REFERENCE_ACQUIRE)
        request->reference = state->reference;
    if (action == MEM_SERVICE_REFERENCE_ACQUIRE) request->access = LINGQU_OBJECT_REF_V2_READ;
    uint64_t started = now_ms();
    int rc;
    do {
        status = MEM_SERVICE_WIRE_STATUS_INTERNAL;
        rc = mem_service_client_reference_transition(&state->client, request, &result, &status);
        if (!rc) break;
        /* A staged record is invisible until SEAL; the wire reports that
         * content-state conflict as STALE_REF. Only this read-only lookup polls. */
        if (action != MEM_SERVICE_REFERENCE_RESOLVE ||
            (status != MEM_SERVICE_WIRE_STATUS_NOT_FOUND && status != MEM_SERVICE_WIRE_STATUS_STALE_REF))
            return rc;
        usleep(10000);
    } while (now_ms() - started < state->config.timeout_ms);
    if (rc) return -ETIMEDOUT;
    state->allocation = result.allocation;
    if (action == MEM_SERVICE_REFERENCE_RESOLVE) state->reference = result.reference;
    if (action == MEM_SERVICE_REFERENCE_ACQUIRE) state->held = true;
    printf("MANAGED_PTO_REFERENCE node=%u action=%" PRIu64 " generation=%" PRIu64
           " version=%" PRIu64 " status=ok object=%s view=%u\n", state->config.node, action,
           state->allocation.generation, state->allocation.version,
           object_label(state), state->output_view);
    return 0;
}

static int publish_reference_input(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char operation[80];
    int rc = reference_transition(state, MEM_SERVICE_REFERENCE_BEGIN);
    if (!rc) rc = mapping_operation(operation);
    if (!rc) rc = mem_service_client_map_managed_allocation(&state->client, &state->channel,
        &state->allocation, state->session, operation,
        MEM_SERVICE_CLIENT_MAP_READ | MEM_SERVICE_CLIENT_MAP_WRITE,
        &state->mapping, &state->lifecycle, &status);
    if (!rc) rc = seed_inputs_callback(state, &state->channel, &state->mapping,
                                       &state->lifecycle, NULL);
    struct mem_service_client_reference_view view = {
        .offset = vector_offset(state, 0),
        .len = vector_offset(state, 1) + state->config.geometry.tensor_bytes - vector_offset(state, 0),
        .object_kind = 1, .owner_entity = state->config.home_node,
        .producer_entity = state->config.home_node,
    };
    if (state->config.model_probe) {
        view.offset = 0;
        view.len = MODEL_HIDDEN_BYTES;
        view.object_kind = MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT;
    }
    if (!rc) rc = mem_service_client_prepare_managed_reference(&state->client, &state->channel,
        &state->allocation, &state->mapping, &state->lifecycle, &view, &state->reference, &status);
    if (!rc) rc = reference_transition(state, MEM_SERVICE_REFERENCE_STAGE);
    if (!rc) rc = mem_service_client_unmap_managed_allocation(&state->client, &state->channel,
        &state->mapping, &state->lifecycle, &status);
    if (!rc) rc = reference_transition(state, MEM_SERVICE_REFERENCE_SEAL);
    return rc;
}

static int map_reference_input(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char operation[80], encoded[513], expected_home[32];
    struct lingqu_object_ref_wire_v2 published = state->reference;
    uint64_t generation = state->allocation.generation;
    uint64_t incarnation = state->allocation.provider_incarnation;
    int rc = reference_transition(state, MEM_SERVICE_REFERENCE_RESOLVE);
    if (rc) return rc;
    snprintf(expected_home, sizeof(expected_home), "guest-node-%u", state->config.home_node);
    const struct lingqu_object_ref_wire_v2 *reference = &state->reference;
    if (strcmp(reference->allocation_key, state->config.key) ||
        reference->allocation_generation != generation || reference->provider_incarnation != incarnation ||
        strcmp(reference->home_node, expected_home) ||
        reference->allocation_bytes != state->config.geometry.object_bytes ||
        reference->object.payload_offset != (state->config.model_probe ? 0 : vector_offset(state, 0)) ||
        reference->object.payload_bytes != (state->config.model_probe ? MODEL_HIDDEN_BYTES :
            vector_offset(state, 1) + state->config.geometry.tensor_bytes - vector_offset(state, 0)) ||
        (state->config.model_probe && reference->object.object_kind !=
            MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT) ||
        reference->access != LINGQU_OBJECT_REF_V2_READ ||
        (state->held && memcmp(&published, reference, sizeof(published)))) return -ESTALE;
    if (!state->held) rc = reference_transition(state, MEM_SERVICE_REFERENCE_ACQUIRE);
    if (!rc) rc = mapping_operation(operation);
    if (!rc) rc = mem_service_client_map_managed_reference(&state->client, &state->channel,
        reference, state->session, operation, &state->mapping, &state->reference_lifecycle, &status);
    if (rc) return rc;
    /* A confirmed active mapping has its exact transaction ID. Move its cleanup
     * obligation to the existing owner path only after successful reference map. */
    state->lifecycle = state->reference_lifecycle.mapping;
    memset(&state->reference_lifecycle, 0, sizeof(state->reference_lifecycle));
    state->view_offset = reference->object.payload_offset;
    rc = mem_service_reference_encode_hex(reference, encoded, sizeof(encoded));
    if (!rc) printf("MANAGED_PTO_REFERENCE node=%u action=mapped reference=%s\n",
                    state->config.node, encoded);
    return rc;
}

static int map_model_output_reference(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char operation[80], encoded[513], home[32];
    const uint64_t generation = state->allocation.generation;
    const uint64_t incarnation = state->allocation.provider_incarnation;
    const unsigned kinds[] = {MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
        MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT, MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE};
    const struct llm_infer_result_view *view = state->output_view == 1 ?
        &state->decoded_output.hidden : &state->decoded_output.kv;
    if (state->output_view >= 3) return -EINVAL;
    uint64_t offset = state->output_view ? view->offset : MODEL_OUTPUT_OFFSET;
    uint64_t length = state->output_view ? view->bytes : state->config.model_output_bytes;
    uint64_t checksum = state->output_view ? view->service_checksum : state->config.model_output_checksum;
    int rc = reference_transition(state, MEM_SERVICE_REFERENCE_RESOLVE);
    if (rc) return rc;
    const struct lingqu_object_ref_wire_v2 *ref = &state->reference;
    snprintf(home, sizeof(home), "guest-node-%u", state->config.home_node);
    if (strcmp(ref->allocation_key, state->config.key) || strcmp(ref->home_node, home) ||
        ref->allocation_generation != generation || ref->provider_incarnation != incarnation ||
        ref->allocation_bytes != state->config.geometry.object_bytes ||
        ref->access != LINGQU_OBJECT_REF_V2_READ || ref->object.payload_offset != offset ||
        ref->object.payload_bytes != length || ref->object.payload_checksum != checksum ||
        ref->object.object_kind != kinds[state->output_view] ||
        ref->object.owner_entity != 0 || ref->object.producer_entity != 0) return -ESTALE;
    if (state->output_view && ref->object.object_version !=
        state->published_outputs[0].object.object_version) return -ESTALE;
    rc = reference_transition(state, MEM_SERVICE_REFERENCE_ACQUIRE);
    if (!rc) rc = mapping_operation(operation);
    if (!rc) rc = mem_service_client_map_managed_reference(&state->client, &state->channel,
        ref, state->session, operation, &state->mapping, &state->reference_lifecycle, &status);
    if (rc) return rc;
    state->lifecycle = state->reference_lifecycle.mapping;
    memset(&state->reference_lifecycle, 0, sizeof(state->reference_lifecycle));
    state->view_offset = offset;
    state->published_outputs[state->output_view] = *ref;
    rc = mem_service_reference_encode_hex(ref, encoded, sizeof(encoded));
    if (!rc) printf("MANAGED_MODEL_REFERENCE node=%u action=mapped view=%u reference=%s"
        " cpu_base=0x%" PRIxPTR " logical_bytes=%" PRIu64 " mapping_id=%" PRIu64 "\n",
        state->config.node, state->output_view, encoded, (uintptr_t)state->mapping.base,
        state->mapping.len, state->lifecycle.transaction.mapping_id);
    return rc;
}

static int map_object(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char idem[80], operation[80], expected_home[32];
    int rc;
    snprintf(expected_home, sizeof(expected_home), "guest-node-%u", state->config.home_node);
    bool publishing_output = state->config.model_publish && state->is_output;
    bool allocator = publishing_output ? state->config.node != state->config.home_node :
                                         state->config.node == state->config.home_node;
    if (allocator) {
        snprintf(idem, sizeof(idem), "pto-alloc-%" PRIu64 "-%u",
                 state->config.generation, state->is_output ? 1 : 0);
        struct mem_service_client_allocate request = {
            .key = state->config.key, .idempotency_key = idem,
            .session_id = state->session, .size_bytes = state->config.geometry.object_bytes,
            .capabilities = MEM_SERVICE_CLIENT_MANAGED_CAP_MAP,
        };
        rc = mem_service_client_allocate_object(&state->client, &request,
                                                &state->allocation, &status);
        if (rc) return rc;
    }
    rc = wait_allocation(state, "active", false);
    if (rc) return rc;
    if (state->allocation.size_bytes != state->config.geometry.object_bytes ||
        !state->allocation.provider_backed || strcmp(state->allocation.home_node, expected_home))
        return -ESTALE;
    bool reference_input = state->config.reference_inputs && !state->is_output;
    if (publishing_output && !allocator) {
        rc = map_model_output_reference(state);
        goto mapped;
    }
    if (reference_input && state->config.node != state->config.home_node) {
        rc = map_reference_input(state);
        goto mapped;
    }
    snprintf(idem, sizeof(idem), "pto-acquire-%s", state->session);
    if (state->config.holder_recovery_probe) {
        char holder_node[32];
        snprintf(holder_node, sizeof(holder_node), "guest-node-%u", state->config.node);
        rc = mem_service_client_acquire_object_at_node(&state->client,
            state->config.key, idem, state->session, holder_node,
            state->config.generation, true, state->allocation.generation,
            &state->allocation, &status);
    } else {
        rc = mem_service_client_acquire_object(&state->client, state->config.key, idem,
            state->session, true, state->allocation.generation,
            &state->allocation, &status);
    }
    if (rc) return rc;
    state->held = true;
    if (strcmp(state->allocation.home_node, expected_home)) return -ESTALE;
    if (reference_input) {
        rc = publish_reference_input(state);
        if (!rc) rc = map_reference_input(state);
        goto mapped;
    }
    if (publishing_output) {
        rc = reference_transition(state, MEM_SERVICE_REFERENCE_BEGIN);
        if (rc) return rc;
    }
    rc = mapping_operation(operation);
    if (rc) return rc;
    rc = mem_service_client_map_managed_allocation(&state->client, &state->channel,
        &state->allocation, state->session, operation,
        mapping_access(state),
        &state->mapping, &state->lifecycle, &status);
mapped:
    if (!rc) printf("MANAGED_PTO node=%u key=%s generation=%" PRIu64
        " cpu_base=0x%" PRIx64 " address=0x%" PRIx64 " logical_bytes=%" PRIu64
        " mapping_id=%" PRIu64 " mapping_handle=%" PRIu64 " access=%" PRIu64
        " object=%s home_node=%u home_provider=%s requester_cna=0x%x\n",
        state->config.node, state->mapping.key, state->mapping.generation,
        (uint64_t)(uintptr_t)state->mapping.base, state->allocation.address,
        state->mapping.len, state->lifecycle.transaction.mapping_id,
        state->mapping.binding.mapping.handle, state->mapping.flags, object_label(state),
        state->config.home_node, state->allocation.home_node, state->config.requester);
    return rc;
}

static int release_compute(struct managed_state *state)
{
    if (state->completion_unknown) return -EBUSY;
    if (state->model_operation) {
        int rc = w5_managed_model_release(state->model_operation);
        if (rc) return rc;
        state->model_operation = NULL;
    }
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
    uint64_t mapped_bytes = state->mapping.len;
    snprintf(release_operation, sizeof(release_operation), "pto-release-%s", state->session);
    state->mapping_id = state->lifecycle.transaction.mapping_id;
    int rc = 0;
    if (!state->domain) {
        rc = mem_service_access_domain_create(&state->client, &state->channel, &state->domain);
        if (!rc) state->owns_domain = true;
    }
    if (!rc) rc = mem_service_mapping_owner_adopt(state->domain, &state->mapping,
        &state->lifecycle, release_operation, MAX_ACCESS_THREADS +
        (state->config.concurrent_requests == 2 ? 7 : 4), &state->owner, &status);
    if (rc) return rc;
    state->held = false; /* This exact holder now belongs to the SDK owner. */
    struct mem_service_mapping_access_view view;
    rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        0, mapped_bytes, mapping_access(state),
        &state->cpu_pin, &view);
    if (!rc) printf("MANAGED_OWNER node=%u state=adopted generation=%" PRIu64
        " mapping_id=%" PRIu64 " cpu_pins=1 holder_transferred=1 object=%s\n",
        state->config.node, state->allocation.generation, state->mapping_id, object_label(state));
    return rc;
}

static int model_output_key(const struct managed_config *config,
    char *out, size_t size)
{
    if (config->model_publish) {
        /* This diagnostic is a fixed two-node, step-zero producer/reader pair.
         * Both guests must choose the producer's node, never their own node. */
        return llm_infer_managed_object_key(out, size, config->generation,
            config->generation + 1U, 1U - config->home_node, 0);
    }
    int length = snprintf(out, size, "%s-output", config->key);
    return length < 0 || (size_t)length >= size ? -ENAMETOOLONG : 0;
}

static int open_output_object(struct managed_state *state)
{
    struct managed_state *output = calloc(1, sizeof(*output));
    if (!output) return -ENOMEM;
    state->output = output;
    output->config = state->config;
    output->is_output = true;
    output->client = state->client;
    output->channel = state->channel;
    output->domain = state->domain;
    int rc = model_output_key(&state->config, output->config.key,
        sizeof(output->config.key));
    if (rc) return rc;
    int length = snprintf(output->session, sizeof(output->session),
                      "managed-pto-out-%u-%" PRIu64, state->config.node, state->config.generation);
    if (length < 0 || (size_t)length >= sizeof(output->session)) return -ENAMETOOLONG;
    rc = map_object(output);
    if (!rc) rc = adopt_mapping(output);
    if (!rc && (output->domain != state->domain || output->owns_domain ||
        output->mapping_id == state->mapping_id ||
        output->allocation.address == state->allocation.address)) rc = -EPROTO;
    if (!rc) printf("MANAGED_OWNER_PAIR node=%u shared_domain=1 input_mapping_id=%" PRIu64
        " output_mapping_id=%" PRIu64 "\n", state->config.node,
        state->mapping_id, output->mapping_id);
    return rc;
}

static void *cpu_worker(void *opaque)
{
    struct managed_state *state = opaque;
    struct mem_service_mapping_lease *pin = NULL;
    struct mem_service_mapping_access_view view;
    int rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        vector_offset(state, 0), state->config.geometry.tensor_bytes,
        MEM_SERVICE_CLIENT_MAP_READ, &pin, &view);
    bool ready = false, observed_seal = false;
    do {
        pthread_mutex_lock(&state->workers_lock);
        bool stop = state->workers_stop, sealed = state->owner_sealed;
        pthread_mutex_unlock(&state->workers_lock);
        if (stop) break;
        if (!rc) {
            const volatile float *actual = view.base;
            for (unsigned i = 0; i < state->config.geometry.elements; ++i)
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
    unsigned expected_compute = !in_flight ? 0 : !state->config.readonly_inputs ? 3 :
                                state->is_output ? 2 : 1;
    if (in_flight && state->config.concurrent_requests == 2)
        expected_compute = !state->config.readonly_inputs ? 6 : state->is_output ? 2 : 4;
    if (!stats.closing || !stats.mapping_pending || !stats.holder_pending ||
        stats.cpu_pins != 1 + state->workers_started ||
        stats.compute_pins != expected_compute || stats.in_flight_pins != expected_compute)
        return -EPROTO;
    struct mem_service_mapping_lease *blocked = NULL;
    struct mem_service_mapping_access_view view;
    rc = mem_service_mapping_owner_acquire(state->owner, MEM_SERVICE_MAPPING_ACCESS_CPU,
        vector_offset(state, 0), state->config.geometry.tensor_bytes,
        MEM_SERVICE_CLIENT_MAP_READ, &blocked, &view);
    if (rc != -ECANCELED || blocked) return -EPROTO;
    if (state->is_output) {
        state->owner_sealed = true;
        rc = 0;
    } else {
        pthread_mutex_lock(&state->workers_lock);
        state->owner_sealed = true;
        while (state->workers_observed_seal < state->workers_started && !state->worker_error)
            pthread_cond_wait(&state->workers_condition, &state->workers_lock);
        rc = state->worker_error;
        pthread_mutex_unlock(&state->workers_lock);
    }
    if (!rc) printf("MANAGED_OWNER node=%u state=closing cpu_pins=%u compute_pins=%u "
        "in_flight_pins=%u close_guard=pass admission_guard=pass threads=%u operation_id=%" PRIu64
        " object=%s\n",
        state->config.node, stats.cpu_pins, stats.compute_pins, stats.in_flight_pins, state->workers_started,
        in_flight ? state->config.generation + 2 : 0, object_label(state));
    if (!rc && state->output) rc = probe_owner_close(state->output, in_flight);
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
    printf("MANAGED_OWNER node=%u state=workers-drained threads=%u checks=%u "
        "after_seal_threads=%u cpu_verify=pass object=%s\n", state->config.node, state->workers_started,
        state->worker_checks, state->workers_observed_seal, object_label(state));
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
    if (state->output) {
        rc = close_mapping(state->output);
        if (rc) return rc;
    }
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
               "in_flight_pins=0 mapping_pending=0 holder_pending=0 object=%s\n",
               state->config.node, object_label(state));
    }
    if (state->domain) {
        if (state->owns_domain) {
            rc = mem_service_access_domain_destroy(state->domain);
            if (rc) return rc;
            state->owns_domain = false;
        }
        state->domain = NULL;
    }
    if (state->lifecycle.pending || state->reference_lifecycle.mapping.pending ||
        state->mapping.binding.mapped) {
        if (state->reference_lifecycle.mapping.pending) {
            rc = mem_service_client_unmap_managed_reference(&state->client, &state->channel,
                &state->mapping, &state->reference_lifecycle, &status);
            if (rc) return rc;
        } else {
            rc = mem_service_client_unmap_managed_allocation(&state->client, &state->channel,
                &state->mapping, &state->lifecycle, &status);
            if (rc) return rc;
        }
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
    case 3: refs[2].byte_offset = state->config.geometry.object_bytes -
                                state->config.geometry.tensor_bytes + 1; break;
    case 4: refs[2].access = LINGQU_PTO_UB_GM_READ; break;
    /* Execution probes use explicitly marked kernels and ordinary metadata. */
    case 5: case 6: break;
    default: return -EINVAL;
    }
    return lingqu_shmem_pto_refresh_fault_metadata_crc(state->metadata,
        METADATA_BYTES, metadata_iova, wire);
}

/* Called with the batch lock held; always releases it before returning. */
static int queued_wait_locked(struct managed_state *state, const LingquPtoDispatchSlotV2 *slot,
                              uint64_t op_id, struct lingqu_shmem_pto_completion *completion)
{
    struct managed_batch *batch = state->batch;
    int rc = lingqu_shmem_pto_endpoint_enqueue(state->endpoint, slot);
    if (!rc) {
        ++batch->submitted;
        printf("MANAGED_PTO_QUEUE node=%u state=queued dispatch=%u op_id=%" PRIu64
            " queued_requests=%u retained_leases=3 metadata_iova=0x%" PRIx64 "\n",
            state->config.node, state->request_round, op_id,
            lingqu_shmem_pto_endpoint_queued_count(state->endpoint), state->metadata_iova);
        pthread_cond_broadcast(&batch->changed);
        while (!batch->allow_poll && !batch->error)
            pthread_cond_wait(&batch->changed, &batch->lock);
        if (batch->error) rc = batch->error;
    }
    uint64_t started = now_ms();
    if (!rc && !started) rc = -EIO;
    while (!rc) {
        rc = lingqu_shmem_pto_endpoint_poll(state->endpoint, op_id, completion);
        if (!rc) {
            printf("MANAGED_PTO_QUEUE node=%u state=completed dispatch=%u op_id=%" PRIu64
                " queued_requests=%u\n", state->config.node, state->request_round, op_id,
                lingqu_shmem_pto_endpoint_queued_count(state->endpoint));
            break;
        }
        if (rc != -EAGAIN) break;
        if (now_ms() - started >= state->config.timeout_ms) { rc = -ETIMEDOUT; break; }
        pthread_mutex_unlock(&batch->lock);
        usleep(1000);
        pthread_mutex_lock(&batch->lock);
        rc = batch->error;
    }
    pthread_mutex_unlock(&batch->lock);
    return rc;
}

static int allocate_metadata(struct managed_state *state)
{
    state->metadata = mmap(NULL, METADATA_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (state->metadata == MAP_FAILED) { state->metadata = NULL; return -errno; }
    memset(state->metadata, 0, METADATA_BYTES);
    return mlock(state->metadata, METADATA_BYTES) ? -errno : 0;
}

static int open_compute(struct managed_state *state)
{
    int rc = lingqu_shmem_mem_service_open_managed(&state->compute);
    if (rc || state->config.concurrent_requests == 2) return rc;
    return allocate_metadata(state);
}

static int dispatch_vector_checked(struct managed_state *state, unsigned round, unsigned fault)
{
    struct lingqu_shmem_memref *memrefs[3] = {0};
    struct lingqu_shmem_pto_memref_arg args[3];
    struct lingqu_shmem_pto_wire_result wire;
    struct lingqu_shmem_pto_completion completion;
    LingquPtoDispatchSlotV2 slot;
    uint64_t metadata_iova;
    unsigned vectors[3] = {round == 1 || state->config.concurrent_requests == 2 ? 0 : 2,
                          1, round + 1};
    const bool forced_revoke = state->config.holder_recovery_probe;
    int rc;
    for (unsigned i = 0; i < 3; ++i) {
        struct lingqu_shmem_memref_spec spec = {
            .byte_offset = vector_offset(vector_state(state, vectors[i]), vectors[i]),
            .byte_length = state->config.geometry.tensor_bytes,
            .rank = 2, .dtype = 0,
            .shape = {state->config.geometry.rows, state->config.geometry.columns},
            .strides = {state->config.geometry.columns, 1},
            .access = i == 2 ? LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ,
        };
        rc = lingqu_shmem_mem_service_acquire_owned(state->compute,
            vector_state(state, vectors[i])->owner,
            &spec, &memrefs[i], &state->leases[i]);
        if (rc) return rc;
        args[i] = (struct lingqu_shmem_pto_memref_arg) {.memref = memrefs[i], .arg_index = i};
    }
    rc = lingqu_shmem_sim_phys_for_virt(state->metadata, &metadata_iova);
    if (rc) return rc;
    state->metadata_iova = metadata_iova;
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
    if (!state->batch && lingqu_shmem_mem_service_active_leases(state->compute) != 3)
        return -EPROTO;
    for (unsigned i = 0; i < 3; ++i) {
        rc = lingqu_shmem_mem_service_lease_begin(state->leases[i], request.op_id);
        if (rc) return rc;
    }
    if (forced_revoke) {
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=prepared op_id=%" PRIu64
            " generation=%" PRIu64 " mapping_id=%" PRIu64
            " holder_node=guest-node-%u holder_incarnation=%" PRIu64
            " retained_leases=3\n",
            state->config.node, request.op_id, state->allocation.generation,
            state->mapping_id, state->config.node, state->config.generation);
        rc = wait_holder_recovery_retired(state);
        if (rc) return rc;
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=retired op_id=%" PRIu64
            " generation=%" PRIu64 " mapping_id=%" PRIu64
            " retained_leases=3\n",
            state->config.node, request.op_id, state->allocation.generation,
            state->mapping_id);
    }
    if (state->batch) {
        pthread_mutex_lock(&state->batch->lock);
        while (state->batch->submitted != round - 1 && !state->batch->error)
            pthread_cond_wait(&state->batch->changed, &state->batch->lock);
        if (state->batch->error) {
            rc = state->batch->error;
            pthread_mutex_unlock(&state->batch->lock);
            return rc;
        }
    }
    const char *expected_error = fault == 2 || fault >= 5 ?
        LINGQU_PTO_UB_GM_CODE_ACCESS_DENIED : LINGQU_PTO_UB_GM_CODE_BAD_MEMREF;
    if (fault) {
        printf("MANAGED_PTO_NEGATIVE node=%u case=%s state=submitted op_id=%" PRIu64
            " mapping_id=%" PRIu64 " expected=%s metadata_crc=0x%08x retained_leases=3\n",
            state->config.node, negative_names[fault - 1], request.op_id, state->mapping_id,
            expected_error, wire.metadata_crc32);
    } else printf("MANAGED_PTO node=%u dispatch=%u op_id=%" PRIu64 " generation=%" PRIu64
        " mapping_id=%" PRIu64 " offsets=%" PRIu64 ",%" PRIu64 ",%" PRIu64
        " tensor_bytes=%zu retained_leases=3 release_guard=pass object_maps=%" PRIu64
        ",%" PRIu64 ",%" PRIu64 "\n",
        state->config.node, round, request.op_id, state->allocation.generation,
        state->mapping_id, vector_offset(vector_state(state, vectors[0]), vectors[0]),
        vector_offset(vector_state(state, vectors[1]), vectors[1]),
        vector_offset(vector_state(state, vectors[2]), vectors[2]),
        (size_t)state->config.geometry.tensor_bytes,
        vector_state(state, vectors[0])->mapping_id,
        vector_state(state, vectors[1])->mapping_id,
        vector_state(state, vectors[2])->mapping_id);
    if (forced_revoke) {
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=submit-start"
            " op_id=%" PRIu64 " generation=%" PRIu64
            " mapping_id=%" PRIu64 " retained_cpu_pins=1 retained_leases=3\n",
            state->config.node, request.op_id, state->allocation.generation,
            state->mapping_id);
    }
    state->completion_unknown = true;
    rc = state->batch ? queued_wait_locked(state, &slot, request.op_id, &completion) :
        lingqu_shmem_pto_endpoint_submit(state->endpoint, &slot,
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
        printf("MANAGED_PTO_WAIT node=%u dispatch=%u op_id=%" PRIu64
            " state=timeout retained_leases=3 close_guard=pass release_guard=pass\n",
            state->config.node, round, request.op_id);
        rc = lingqu_shmem_pto_endpoint_wait(state->endpoint, request.op_id,
                                            state->config.timeout_ms, &completion);
        if (!rc) {
            if (lingqu_shmem_pto_endpoint_pending_op(state->endpoint)) return -EPROTO;
            recovered = true;
            printf("MANAGED_PTO_WAIT node=%u dispatch=%u op_id=%" PRIu64
                " state=recovered completion_op=%" PRIu64 " pending_op=0\n",
                state->config.node, round, request.op_id, completion.op_id);
        }
    }
    if (rc) return rc;
    if (completion.op_id != request.op_id) return -EPROTO;
    for (unsigned i = 0; i < 3; ++i) {
        rc = lingqu_shmem_mem_service_lease_complete(state->leases[i], request.op_id);
        if (rc) return rc;
    }
    state->completion_unknown = false;
    if (forced_revoke) {
        struct lingqu_shmem_pto_cpu_fault cpu_fault;
        if (completion.status != 3 ||
            (strcmp(completion.error_code, LINGQU_PTO_UB_GM_CODE_ACCESS_DENIED) &&
             strcmp(completion.error_code, LINGQU_PTO_UB_GM_CODE_BAD_MEMREF)) ||
            lingqu_shmem_pto_endpoint_pending_op(state->endpoint)) return -EPROTO;
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=rejected op_id=%" PRIu64
            " completion_op=%" PRIu64 " status=%u error=%s pending_op=0"
            " cleanup=process-exit\n",
            state->config.node, request.op_id, completion.op_id,
            completion.status, completion.error_code);
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=cpu-probe-start"
            " op_id=%" PRIu64 " generation=%" PRIu64
            " mapping_id=%" PRIu64 " retained_cpu_pins=1 retained_leases=3\n",
            state->config.node, request.op_id, state->allocation.generation,
            state->mapping_id);
        rc = lingqu_shmem_pto_probe_revoked_read(state->mapping.base, &cpu_fault);
        if (rc || (cpu_fault.signal != SIGBUS && cpu_fault.signal != SIGSEGV))
            return rc ? rc : -EPROTO;
        state->holder_recovery_cpu_signal = cpu_fault.signal;
        printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=cpu-rejected"
            " op_id=%" PRIu64 " generation=%" PRIu64
            " mapping_id=%" PRIu64 " signal=%d code=%d address=0x%" PRIxPTR
            " retained_cpu_pins=1 retained_leases=3\n",
            state->config.node, request.op_id, state->allocation.generation,
            state->mapping_id, cpu_fault.signal, cpu_fault.code,
            cpu_fault.address);
        lingqu_shmem_pto_dispatch_finish(state->inflight);
        state->inflight = NULL;
        return 0;
    }
    if (fault) {
        if (completion.status != 3 || strcmp(completion.error_code, expected_error) ||
            lingqu_shmem_pto_endpoint_pending_op(state->endpoint)) return -EPROTO;
        printf("MANAGED_PTO_NEGATIVE node=%u case=%s state=rejected op_id=%" PRIu64
            " completion_op=%" PRIu64 " status=%u error=%s pending_op=0\n",
            state->config.node, negative_names[fault - 1], request.op_id, completion.op_id,
            completion.status, completion.error_code);
        rc = release_compute(state);
        for (unsigned vector = 0; !rc && vector < 4; ++vector) {
            struct managed_state *object = vector_state(state, vector);
            struct vector_check check = {object, vector, negative_names[fault - 1], request.op_id};
            rc = mem_service_mapping_lease_apply(object->cpu_pin, verify_vector_callback, &check);
        }
        return rc;
    }
    if (state->config.recovery_expected && !recovered) return -EPROTO;
    printf("MANAGED_PTO node=%u dispatch=%u completion_op=%" PRIu64
        " status=%u error=%s\n", state->config.node, round, completion.op_id, completion.status,
        completion.error_code[0] ? completion.error_code : "none");
    if (!lingqu_shmem_pto_completion_succeeded(&completion)) return -EIO;
    rc = release_compute(state);
    if (rc) return rc;
    if (state->batch) return 0; /* Parent verifies C then D after both callers join. */
    return verify_vector(state, vectors[2], round == 1 ? "tstore-cpu" : "tstore-tload-cpu");
}

static int dispatch_vector(struct managed_state *state, unsigned round)
{
    return dispatch_vector_checked(state, round, 0);
}

static void *compute_caller(void *opaque)
{
    struct managed_state *request = opaque;
    int rc = dispatch_vector(request, request->request_round);
    pthread_mutex_lock(&request->batch->lock);
    if (rc && !request->batch->error) request->batch->error = rc;
    pthread_cond_broadcast(&request->batch->changed);
    pthread_mutex_unlock(&request->batch->lock);
    return NULL;
}

/* Both submitted callers are gated here, so none can consume a completion. */
static int probe_batch(struct managed_state *state)
{
    struct managed_batch *batch = state->batch;
    if (batch->submitted != 2 || batch->started != 2 ||
        lingqu_shmem_pto_endpoint_queued_count(state->endpoint) != 2 ||
        lingqu_shmem_pto_endpoint_close_checked(state->endpoint) != -EBUSY)
        return -EPROTO;
    struct managed_state *first = batch->requests[0], *second = batch->requests[1];
    if (!state->compute || first->compute != state->compute || second->compute != state->compute ||
        lingqu_shmem_mem_service_active_leases(state->compute) != 6 ||
        first->metadata == second->metadata ||
        !first->metadata_iova || !second->metadata_iova ||
        first->metadata_iova / METADATA_BYTES == second->metadata_iova / METADATA_BYTES)
        return -EPROTO;
    for (unsigned i = 0; i < 2; ++i) {
        struct managed_state *request = batch->requests[i];
        if (!request->completion_unknown ||
            release_compute(request) != -EBUSY) return -EPROTO;
        for (unsigned j = 0; j < 3; ++j) {
            int rc = lingqu_shmem_mem_service_release(request->leases[j]);
            if (rc != -EBUSY) {
                if (!rc) request->leases[j] = NULL;
                return -EPROTO;
            }
        }
    }
    int rc = probe_owner_close(state, true);
    if (!rc) printf("MANAGED_PTO_BATCH node=%u state=overlap callers=2 queued_requests=2 "
        "retained_leases=6 metadata_distinct=1 close_guard=pass release_guard=pass "
        "op_ids=%" PRIu64 ",%" PRIu64 "\n", state->config.node,
        state->config.generation + 1, state->config.generation + 2);
    return rc;
}

static int dispatch_batch(struct managed_state *state)
{
    struct managed_batch *batch = calloc(1, sizeof(*batch));
    if (!batch) return -ENOMEM;
    state->batch = batch;
    int rc = pthread_mutex_init(&batch->lock, NULL);
    if (rc) return -rc;
    rc = pthread_cond_init(&batch->changed, NULL);
    if (rc) return -rc;
    for (unsigned i = 0; i < 2; ++i) {
        struct managed_state *request = calloc(1, sizeof(*request));
        if (!request) return -ENOMEM;
        batch->requests[i] = request;
        request->config = state->config;
        request->allocation = state->allocation;
        request->mapping_id = state->mapping_id;
        request->view_offset = state->view_offset;
        request->owner = state->owner;
        request->output = state->output;
        request->domain = state->domain;
        request->endpoint = state->endpoint;
        request->compute = state->compute; /* Borrowed until both callers join. */
        request->batch = batch;
        request->request_round = i + 1;
        rc = allocate_metadata(request);
        if (rc) return rc;
    }
    for (unsigned i = 0; i < 2; ++i) {
        rc = pthread_create(&batch->threads[i], NULL, compute_caller, batch->requests[i]);
        if (rc) break;
        ++batch->started;
    }
    pthread_mutex_lock(&batch->lock);
    if (rc && !batch->error) batch->error = -rc;
    while (batch->submitted < 2 && !batch->error)
        pthread_cond_wait(&batch->changed, &batch->lock);
    if (!batch->error) batch->error = probe_batch(state);
    batch->allow_poll = true;
    pthread_cond_broadcast(&batch->changed);
    pthread_mutex_unlock(&batch->lock);
    for (unsigned i = 0; i < batch->started; ++i) {
        rc = pthread_join(batch->threads[i], NULL);
        if (rc) return -rc;
    }
    state->completion_unknown = batch->requests[0]->completion_unknown ||
                                batch->requests[1]->completion_unknown;
    if (batch->error) return batch->error;
    if (state->completion_unknown || lingqu_shmem_pto_endpoint_queued_count(state->endpoint))
        return -EPROTO;
    if (lingqu_shmem_mem_service_active_leases(state->compute)) return -EPROTO;
    for (unsigned i = 0; i < 2; ++i) {
        struct managed_state *request = batch->requests[i];
        request->compute = NULL;
        if (munmap(request->metadata, METADATA_BYTES)) return -errno;
        free(request);
        batch->requests[i] = NULL;
    }
    printf("MANAGED_PTO_BATCH node=%u state=drained joined_callers=2 queued_requests=0 "
        "compute_leases=0\n", state->config.node);
    pthread_cond_destroy(&batch->changed);
    pthread_mutex_destroy(&batch->lock);
    free(batch);
    state->batch = NULL;
    rc = verify_vector(state, 2, "tstore-cpu");
    if (!rc) rc = verify_vector(state, 3, "tstore-cpu-independent");
    return rc;
}

static int readonly_cpu_probe(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct managed_state *state = opaque;
    struct lingqu_shmem_pto_cpu_fault fault;
    (void)channel; (void)lifecycle;
    if (mapping->flags != MEM_SERVICE_CLIENT_MAP_READ ||
        view->flags != MEM_SERVICE_CLIENT_MAP_READ) return -EPROTO;
    uint8_t *target = (uint8_t *)mapping->base + vector_offset(state, 0);
    int rc = lingqu_shmem_pto_probe_readonly(target, &fault);
    if (rc) return rc;
    const volatile float *input = (const volatile float *)target;
    for (unsigned i = 0; i < state->config.geometry.elements; ++i)
        if (input[i] != expected_value(0, i)) return -EIO;
    state->readonly_checksum = mem_service_provider_checksum64(mapping->base, mapping->len);
    printf("MANAGED_PTO_READONLY node=%u object=input probe=cpu-write-denied "
        "address=0x%" PRIxPTR " signal=%d code=%d unchanged=pass checksum=%016" PRIx64 "\n",
        state->config.node, fault.address, fault.signal, fault.code, state->readonly_checksum);
    return 0;
}

static int readonly_input_unchanged(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct managed_state *state = opaque;
    (void)channel; (void)lifecycle; (void)view;
    uint64_t checksum = mem_service_provider_checksum64(mapping->base, mapping->len);
    if (checksum != state->readonly_checksum) return -EIO;
    printf("MANAGED_PTO_READONLY node=%u object=input probe=input-unchanged "
        "bytes=%" PRIu64 " checksum=%016" PRIx64 " unchanged=pass\n",
        state->config.node, mapping->len, checksum);
    return 0;
}

static int probe_readonly_input(struct managed_state *state)
{
    const enum mem_service_mapping_access_kind kinds[] = {
        MEM_SERVICE_MAPPING_ACCESS_CPU, MEM_SERVICE_MAPPING_ACCESS_COMPUTE,
    };
    for (unsigned i = 0; i < 2; ++i) {
        struct mem_service_mapping_lease *blocked = NULL;
        struct mem_service_mapping_access_view view;
        int rc = mem_service_mapping_owner_acquire(state->owner, kinds[i],
            vector_offset(state, 0), state->config.geometry.tensor_bytes,
            MEM_SERVICE_CLIENT_MAP_WRITE, &blocked, &view);
        if (rc != -EACCES || blocked) return -EPROTO;
    }
    struct mem_service_mapping_owner_stats stats;
    int rc = mem_service_mapping_owner_inspect(state->owner, &stats);
    if (rc || stats.cpu_pins != 1 || stats.compute_pins || stats.in_flight_pins ||
        stats.closing || !stats.mapping_pending || !stats.holder_pending) return -EPROTO;
    printf("MANAGED_PTO_READONLY node=%u object=input probe=sdk-write-denied "
           "cpu_rc=%d compute_rc=%d cpu_pins=1 compute_pins=0 in_flight_pins=0\n",
           state->config.node, -EACCES, -EACCES);
    return mem_service_mapping_lease_apply(state->cpu_pin, readonly_cpu_probe, state);
}

struct model_access {
    struct managed_state *state;
    unsigned action; /* 0: prepare control, 1: publish output, 2: verify output. */
};

static int stage_model_output(struct managed_state *output,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle)
{
    const struct llm_infer_result_view *decoded[] = {
        NULL, &output->decoded_output.hidden, &output->decoded_output.kv,
    };
    const unsigned kinds[] = {MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT,
        MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT, MEM_SERVICE_OBMM_KIND_MODEL_KV_STATE};
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t result_bytes = output->decoded_output.serialized_bytes;
        struct mem_service_client_reference_view view = {
            .offset = i ? decoded[i]->offset : MODEL_OUTPUT_OFFSET,
            .len = i ? decoded[i]->bytes : result_bytes,
            .object_kind = kinds[i], .owner_entity = output->config.node,
            .producer_entity = output->config.node,
        };
        enum mem_service_wire_status status;
        int rc = mem_service_client_prepare_managed_reference(&output->client, channel,
            &output->allocation, mapping, lifecycle, &view, &output->reference, &status);
        if (rc) return rc;
        uint64_t checksum = i ? decoded[i]->service_checksum :
            output->decoded_output.serialized_checksum;
        if (output->reference.object.payload_checksum != checksum) return -EIO;
        output->output_view = i;
        rc = reference_transition(output, MEM_SERVICE_REFERENCE_STAGE);
        if (rc) return rc;
        output->published_outputs[i] = output->reference;
        char encoded[513];
        rc = mem_service_reference_encode_hex(&output->reference, encoded, sizeof(encoded));
        if (rc) return rc;
        printf("MANAGED_MODEL_REFERENCE node=%u action=staged view=%u reference=%s\n",
            output->config.node, i, encoded);
    }
    return 0;
}

static int model_output_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct model_access *access = opaque;
    struct managed_state *state = access->state;
    struct mem_service_visibility_completion completion;
    unsigned char *bytes = mapping->base;
    bool reader = state->config.model_publish && state->config.node == state->config.home_node;
    uint64_t data_offset = reader ? 0 : MODEL_OUTPUT_OFFSET;
    (void)view;
    if (mapping->len != (reader ? state->config.model_output_bytes :
                                 state->config.geometry.object_bytes)) return -EINVAL;
    if (access->action == 0) {
        unsigned char control[W5_MANAGED_MODEL_CONTROL_BYTES];
        memset(control, 0xa5, sizeof(control));
        int rc = mem_service_provider_channel_wait_range_visible(channel, &mapping->binding,
            0, sizeof(control), mem_service_provider_checksum64(control, sizeof(control)),
            state->config.timeout_ms, &completion);
        if (rc) return rc;
        unsigned char prefix[72] = {0};
        memcpy(prefix, "Q3PROMPT", 8);
        prefix[24] = 1; /* One prompt token, decode step zero. */
        prefix[64] = 42;
        const uint32_t levels[8] = {0x5133060b, 0, 1, 2, 1, 2, 28, MODEL_HIDDEN_BYTES};
        rc = w5_managed_model_control_encode(control, sizeof(control),
            state->config.generation + 1, state->config.generation + 1, levels,
            prefix, sizeof(prefix), &state->reference, 1);
        if (rc) return rc;
        memcpy(bytes, control, sizeof(control));
        return mem_service_provider_channel_publish_range(channel, &mapping->binding,
            0, sizeof(control), mem_service_provider_checksum64(control, sizeof(control)),
            &completion);
    }
    uint64_t length = state->config.model_output_bytes;
    uint64_t checksum = state->config.model_output_checksum;
    if (access->action == 1) {
        if (reader || mem_service_provider_checksum64(bytes + data_offset, length) != checksum)
            return -EIO;
        return mem_service_provider_channel_publish_range(channel, &mapping->binding,
            data_offset, length, checksum, &completion);
    }
    int rc = mem_service_provider_channel_wait_range_visible(channel, &mapping->binding,
        data_offset, length, checksum, state->config.timeout_ms, &completion);
    if (rc) return rc;
    if (mem_service_provider_checksum64(bytes + data_offset, length) != checksum)
        return -EIO;
    const struct llm_infer_range_identity expected = {
        .node = 0, .layer_start = 1, .layer_end = 2, .next_node = 1,
        .pipeline_nodes = 2, .total_layers = 28, .hidden_bytes = MODEL_HIDDEN_BYTES,
        .step = 0, .vocab_size = 151936,
    };
    struct llm_infer_range_result result;
    rc = llm_infer_decode_range_result(bytes + data_offset, length,
        MODEL_OUTPUT_OFFSET, &expected, &result);
    if (rc) return rc;
    if (!result.kv.bytes || result.serialized_bytes != length) return -EPROTO;
    state->output->decoded_output = result;
    printf("MANAGED_MODEL_VIEWS node=%u hidden_offset=%" PRIu64
        " hidden_bytes=%" PRIu64 " hidden_checksum=%016" PRIx64
        " kv_offset=%" PRIu64 " kv_bytes=%" PRIu64 " kv_checksum=%016" PRIx64
        " real_layers=%" PRIu64 " serialized_bytes=%" PRIu64
        " serialized_checksum=%016" PRIx64
        " terminal_present=%" PRIu64 " terminal_step=%" PRIu64
        " terminal_token=%" PRIu64
        " payload_copies=0 status=pass\n",
        state->config.node, result.hidden.offset, result.hidden.bytes,
        result.hidden.service_checksum, result.kv.offset, result.kv.bytes,
        result.kv.service_checksum, result.real_layers, result.serialized_bytes,
        result.serialized_checksum,
        result.terminal_present, result.terminal.step, result.terminal.sampled_token);
    /* No write is permitted in the gap or beyond the serialized result. */
    for (uint64_t i = W5_MANAGED_MODEL_CONTROL_BYTES; !reader && i < MODEL_OUTPUT_OFFSET; ++i)
        if (bytes[i] != 0xa5) return -EIO;
    for (uint64_t i = MODEL_OUTPUT_OFFSET + length; !reader && i < mapping->len; ++i)
        if (bytes[i] != 0xa5) return -EIO;
    printf("MANAGED_MODEL node=%u action=cpu-verify bytes=%" PRIu64
        " checksum=%016" PRIx64 " bounds=pass status=pass\n", state->config.node, length, checksum);
    if (state->config.model_publish && !reader)
        return stage_model_output(state->output, channel, mapping, lifecycle);
    return 0;
}

static int model_input_callback(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    const struct managed_state *state = opaque;
    (void)channel; (void)lifecycle;
    /* Adoption moves and clears state->mapping. Inspect the live owner under
     * its CPU lease, never the relinquished source mapping. */
    if (mapping->len != MODEL_HIDDEN_BYTES || view->len != MODEL_HIDDEN_BYTES ||
        mapping->flags != MEM_SERVICE_CLIENT_MAP_READ ||
        view->flags != MEM_SERVICE_CLIENT_MAP_READ || view->offset != 0 ||
        view->base != mapping->base || view->generation != state->allocation.generation)
        return -EPROTO;
    return 0;
}

static int model_reference_read(void *opaque,
    const struct mem_service_provider_channel *channel,
    const struct mem_service_client_object_mapping *mapping,
    const struct mem_service_client_mapping_lifecycle *lifecycle,
    const struct mem_service_mapping_access_view *view)
{
    struct managed_state *output = opaque;
    const struct lingqu_object_ref_wire_v2 *ref = &output->reference;
    struct mem_service_visibility_completion completion;
    if (!lifecycle->pending || mapping->flags != MEM_SERVICE_CLIENT_MAP_READ ||
        view->flags != MEM_SERVICE_CLIENT_MAP_READ || view->offset ||
        view->base != mapping->base || view->len != ref->object.payload_bytes ||
        mapping->len != view->len || view->generation != ref->allocation_generation)
        return -EPROTO;
    int rc = mem_service_provider_channel_wait_range_visible(channel, &mapping->binding,
        0, view->len, ref->object.payload_checksum, output->config.timeout_ms, &completion);
    if (rc) return rc;
    if (mem_service_provider_checksum64(view->base, view->len) != ref->object.payload_checksum)
        return -EIO;
    printf("MANAGED_MODEL_REFERENCE node=%u action=read view=%u bytes=%" PRIu64
        " checksum=%016" PRIx64 " access=1 status=pass\n", output->config.node,
        output->output_view, view->len, ref->object.payload_checksum);
    return 0;
}

static int read_model_output_views(struct managed_state *state)
{
    struct managed_state *output = state->output;
    int rc = mem_service_mapping_lease_apply(output->cpu_pin, model_reference_read, output);
    if (!rc) rc = close_mapping(output);
    for (unsigned i = 1; !rc && i < 3; ++i) {
        output->output_view = i;
        int length = snprintf(output->session, sizeof(output->session),
            "model-reader-%u-%" PRIu64 "-%u", state->config.node, state->config.generation, i);
        if (length < 0 || (size_t)length >= sizeof(output->session)) return -ENAMETOOLONG;
        output->domain = state->domain;
        rc = map_model_output_reference(output);
        if (!rc) rc = adopt_mapping(output);
        if (!rc) rc = mem_service_mapping_lease_apply(output->cpu_pin, model_reference_read, output);
        if (!rc) rc = close_mapping(output);
    }
    return rc;
}

static int seal_model_output(struct managed_state *state)
{
    struct managed_state *output = state->output;
    enum mem_service_wire_status status;
    int rc = mem_service_mapping_lease_release(output->cpu_pin);
    if (rc) return rc;
    output->cpu_pin = NULL;
    rc = mem_service_mapping_owner_unmap(output->owner, &status);
    if (rc) return rc;
    struct mem_service_mapping_owner_stats stats;
    rc = mem_service_mapping_owner_inspect(output->owner, &stats);
    if (rc || !stats.closing || stats.mapping_pending || !stats.holder_pending ||
        stats.cpu_pins || stats.compute_pins || stats.in_flight_pins) return -EPROTO;
    printf("MANAGED_MODEL_PUBLICATION node=%u state=writer-unmapped holder_retained=1\n",
        state->config.node);
    rc = reference_transition(output, MEM_SERVICE_REFERENCE_SEAL);
    if (rc) return rc;
    printf("MANAGED_MODEL_PUBLICATION node=%u state=sealed generation=%" PRIu64
        " version=%" PRIu64 " views=3 payload_copies=0\n", state->config.node,
        output->allocation.generation, output->allocation.version);
    return close_mapping(output);
}

static int run_model_probe(struct managed_state *state)
{
    struct model_access access = {state, 2};
    int rc;
    if (state->config.node == state->config.home_node) {
        rc = state->config.model_publish ? 0 :
            mem_service_mapping_lease_apply(state->output->cpu_pin, seed_inputs_callback, state->output);
        if (!rc) rc = mem_service_mapping_lease_apply(state->output->cpu_pin,
            model_output_callback, &access);
        if (!rc && state->config.model_publish) rc = read_model_output_views(state);
        if (!rc) rc = wait_allocation(state, "active", true);
        if (!rc && !state->config.model_publish) rc = wait_allocation(state->output, "active", true);
        return rc;
    }
    rc = mem_service_mapping_lease_apply(state->cpu_pin, model_input_callback, state);
    if (!rc && state->config.model_publish) rc = mem_service_mapping_lease_apply(
        state->output->cpu_pin, seed_inputs_callback, state->output);
    if (!rc) rc = open_compute(state);
    struct lingqu_shmem_pto_endpoint_info info;
    if (!rc) rc = lingqu_shmem_pto_endpoint_open(&state->endpoint, &info);
    access.action = 0;
    if (!rc) rc = mem_service_mapping_lease_apply(state->output->cpu_pin,
        model_output_callback, &access);
    if (!rc) rc = lingqu_shmem_sim_phys_for_virt(state->metadata, &state->metadata_iova);
    if (rc) return rc;
    struct mem_service_mapping_owner *owners[] = {state->output->owner, state->owner,
                                                state->output->owner};
    const uint64_t offsets[] = {0, 0, MODEL_OUTPUT_OFFSET};
    const uint64_t lengths[] = {W5_MANAGED_MODEL_CONTROL_BYTES, MODEL_HIDDEN_BYTES,
                               state->config.model_output_bytes};
    uint64_t id = state->config.generation + 1;
    rc = w5_managed_model_prepare_views(state->compute, owners, offsets, lengths, 1,
        id, id, state->config.fingerprint, state->config.requester, state->metadata,
        METADATA_BYTES, state->metadata_iova, &state->model_operation);
    if (rc) return rc;
    state->completion_unknown = true;
    rc = w5_managed_model_submit(state->model_operation, state->endpoint);
    if (rc) return rc;
    printf("MANAGED_MODEL node=%u action=submitted op_id=%" PRIu64
        " callable=4 compute_leases=%zu inputs=1\n", state->config.node, id,
        (size_t)lingqu_shmem_mem_service_active_leases(state->compute));
    struct lingqu_shmem_pto_completion completion;
    uint64_t start = now_ms();
    do {
        rc = w5_managed_model_poll(state->model_operation, &completion);
        if (rc != -EAGAIN) break;
        if (now_ms() - start >= state->config.timeout_ms) return -ETIMEDOUT;
        usleep(1000);
    } while (true);
    if (rc) return rc;
    state->completion_unknown = false;
    if (completion.status != 1 || completion.op_id != id) return -EIO;
    printf("MANAGED_MODEL node=%u action=completed op_id=%" PRIu64
        " status=success original_completion=1\n", state->config.node, id);
    rc = release_compute(state);
    if (rc || lingqu_shmem_mem_service_active_leases(state->compute)) return rc ? rc : -EPROTO;
    access.action = 1;
    rc = mem_service_mapping_lease_apply(state->output->cpu_pin, model_output_callback, &access);
    access.action = 2;
    if (!rc) rc = mem_service_mapping_lease_apply(state->output->cpu_pin, model_output_callback, &access);
    if (!rc && state->config.model_publish) rc = seal_model_output(state);
    return rc;
}

static int run_data(struct managed_state *state)
{
    if (state->config.holder_recovery_probe &&
        state->config.node == state->config.home_node) {
        int rc = wait_allocation(state, "active", false);
        if (!rc) {
            printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=peer-ready"
                " generation=%" PRIu64 "\n",
                state->config.node, state->allocation.generation);
            rc = wait_holder_recovery_retired(state);
        }
        if (!rc) printf("MANAGED_PTO_HOLDER_RECOVERY node=%u state=peer-retired"
                        " generation=%" PRIu64 "\n",
                        state->config.node, state->allocation.generation);
        return rc;
    }
    int rc = map_object(state);
    if (!rc) rc = adopt_mapping(state);
    if (!rc && state->config.readonly_inputs) rc = open_output_object(state);
    if (rc) return rc;
    if (state->config.model_probe) return run_model_probe(state);
    if (state->config.node == state->config.home_node) {
        if (!state->config.reference_inputs)
            rc = mem_service_mapping_lease_apply(state->cpu_pin, seed_inputs_callback, state);
        if (!rc && state->output) rc = mem_service_mapping_lease_apply(
            state->output->cpu_pin, seed_inputs_callback, state->output);
        if (!rc) rc = verify_vector(state, 2, "remote-tstore-cpu");
        if (!rc) rc = verify_vector(state, 3, state->config.concurrent_requests == 2 ?
                                    "remote-tstore-cpu-independent" : "remote-tstore-tload-cpu");
        if (!rc) rc = wait_allocation(state, "active", true);
        if (!rc && state->output) rc = wait_allocation(state->output, "active", true);
        return rc;
    }
    if (!state->config.holder_recovery_probe)
        rc = verify_vector(state, 0, "cpu-cpu");
    if (!rc && !state->config.holder_recovery_probe)
        rc = verify_vector(state, 1, "cpu-cpu");
    if (!rc && state->config.readonly_inputs) rc = probe_readonly_input(state);
    if (!rc) rc = open_compute(state);
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    if (!rc) rc = lingqu_shmem_pto_endpoint_open(&state->endpoint, &endpoint_info);
    if (rc) return rc;
    if (state->config.holder_recovery_probe)
        return dispatch_vector(state, 1);
    rc = start_workers(state);
    for (unsigned fault = 1; !rc && state->config.negative_checks && fault <= NEGATIVE_CASES; ++fault)
        rc = dispatch_vector_checked(state, 1, fault);
    if (!rc && state->config.concurrent_requests == 2) rc = dispatch_batch(state);
    else {
        if (!rc) rc = dispatch_vector(state, 1);
        if (!rc) rc = dispatch_vector(state, 2);
    }
    if (!rc && state->config.readonly_inputs) rc = mem_service_mapping_lease_apply(
        state->cpu_pin, readonly_input_unchanged, state);
    if (!rc && !state->owner_sealed) rc = probe_owner_close(state, false);
    if (!rc) rc = stop_workers(state);
    return rc;
}

static int retire_object(struct managed_state *state)
{
    enum mem_service_wire_status status;
    char idem[80];
    snprintf(idem, sizeof(idem), "pto-retire-%" PRIu64 "-%u",
             state->config.generation, state->is_output ? 1 : 0);
    int rc = mem_service_client_retire_object(&state->client, state->config.key, idem, true,
        state->allocation.generation, &state->allocation, &status);
    if (!rc) rc = wait_allocation(state, "retired", false);
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
        if (config.model_publish) {
            char key[MEM_SERVICE_CLIENT_ALLOCATION_KEY_LEN];
            rc = model_output_key(&config, key, sizeof(key));
            if (rc) return 2;
            printf("MANAGED_MODEL_KEY status=ok node=%u producer=%u step=0 key=%s"
                " network_operations=0\n", config.node, 1U - config.home_node, key);
        }
        printf("MANAGED_PTO_CONFIG status=ok node=%u nodes=2 bytes=%u layout=%s "
            "elements=%u callable=%u network_operations=0 submit_timeout_ms=%" PRIu64
            " recovery_expected=%u access_threads=%u negative_checks=%u readonly_inputs=%u home_node=%u"
            " concurrent_requests=%u reference_inputs=%u model_probe=%u model_publish=%u"
            " holder_recovery_probe=%u\n",
            config.node, config.geometry.object_bytes, config.geometry.name,
            config.geometry.elements, config.model_probe ? (unsigned)W5_MANAGED_MODEL_CALLABLE_ID : 1,
            config.submit_timeout_ms,
            config.recovery_expected ? 1 : 0, config.access_threads, config.negative_checks ? 1 : 0,
            config.readonly_inputs ? 1 : 0, config.home_node, config.concurrent_requests,
            config.reference_inputs ? 1 : 0, config.model_probe ? 1 : 0,
            config.model_publish ? 1 : 0, config.holder_recovery_probe ? 1 : 0);
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
    if (config.holder_recovery_probe) {
        if (config.node == config.home_node) {
            stage = "provider-close";
            rc = mem_service_provider_obmm_endpoint_close_checked(&state->provider);
            if (rc) goto failed;
        }
        printf("MANAGED_PTO_RESULT node=%u status=pass key=%s generation=%" PRIu64
            " holder_recovery_probe=1 role=%s old_cpu_access=%s"
            " old_pto_access=%s cpu_signal=%d cleanup=%s\n",
            config.node, config.key, state->allocation.generation,
            config.node == config.home_node ? "peer" : "stale-holder",
            config.node == config.home_node ? "not-applicable" : "rejected",
            config.node == config.home_node ? "not-applicable" : "rejected",
            state->holder_recovery_cpu_signal,
            config.node == config.home_node ? "provider-close" : "process-exit");
        pthread_cond_destroy(&state->workers_condition);
        pthread_mutex_destroy(&state->workers_lock);
        free(state);
        return 0;
    }
    stage = "unmap-release";
    rc = close_mapping(state);
    if (rc) goto failed;
    if (config.node == config.home_node) {
        stage = "retire";
        if (state->output) rc = retire_object(state->output);
        if (!rc) rc = retire_object(state);
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
        " mapping_closed=1 holder_released=1 compute_leases=0 objects=%u readonly_inputs=%u"
        " concurrent_requests=%u reference_inputs=%u\n",
        config.node, config.key, state->allocation.generation,
        state->output ? 2 : 1, config.readonly_inputs ? 1 : 0, config.concurrent_requests,
        config.reference_inputs ? 1 : 0);
    pthread_cond_destroy(&state->workers_condition);
    pthread_mutex_destroy(&state->workers_lock);
    free(state->output);
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
