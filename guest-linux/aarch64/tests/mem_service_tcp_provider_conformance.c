#include "mem_service_provider.h"
#include "mem_service_provider_tcp.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FIXTURE_BYTES 4096U

struct fixture_state {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    uint16_t port;
    bool remote_ready;
    bool client_done;
    int server_rc;
    uint64_t expected_checksum[2];
    uint32_t receive_fence_count;
    struct mem_service_provider_remote_region remote[2];
    uint8_t destination[2][FIXTURE_BYTES];
};

static int bind_endpoint(
    struct mem_service_provider_tcp_endpoint *endpoint,
    struct mem_service_provider_registry *registry,
    struct mem_service_provider_channel *channel)
{
    struct mem_service_provider_registration registration;

    return mem_service_provider_tcp_endpoint_registration(
               endpoint, &registration) != 0 ||
                   mem_service_provider_registry_init(registry) != 0 ||
                   mem_service_provider_registry_register(
                       registry, &registration) != 0 ||
                   mem_service_provider_registry_refresh(registry) != 0 ||
                   mem_service_provider_channel_bind(
                       registry,
                       registration.name,
                       registration.instance,
                       MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
                           MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER,
                       channel) != 0
               ? -1
               : 0;
}

static void *run_server(void *opaque)
{
    struct fixture_state *fixture = opaque;
    struct mem_service_provider_tcp_config config = {
        .local_ipv4 = "127.0.0.1",
        .peer_ipv4 = "127.0.0.1",
        .port = fixture->port,
        .timeout_ms = 10000,
        .receive_on_wait = true,
    };
    struct mem_service_provider_tcp_endpoint endpoint = {0};
    struct mem_service_provider_tcp_canary_result canary;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_region_binding destination[2];
    struct mem_service_region_request request;
    struct mem_service_transfer_completion completion;
    const uint32_t receive_order[] = {0U, 1U, 0U};
    uint32_t i;
    int rc = -1;

    memset(&destination, 0, sizeof(destination));
    if (mem_service_provider_tcp_endpoint_open(
            &endpoint, &config, true) != 0 ||
        mem_service_provider_tcp_endpoint_verify(
            &endpoint, true, FIXTURE_BYTES, 1, &canary) != 0 ||
        !canary.data_plane_ready ||
        bind_endpoint(&endpoint, &registry, &channel) != 0) {
        goto done;
    }
    for (i = 0; i < 2U; ++i) {
        memset(&request, 0, sizeof(request));
        request.base = fixture->destination[i];
        request.len = sizeof(fixture->destination[i]);
        request.memory_kind = MEM_SERVICE_MEMORY_HOST;
        if (mem_service_provider_channel_register_region(
                &channel, &request, &destination[i]) != 0 ||
            mem_service_provider_channel_export_region(
                &channel, &destination[i], &fixture->remote[i]) != 0) {
            goto deregister;
        }
    }
    pthread_mutex_lock(&fixture->lock);
    fixture->remote_ready = true;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->lock);
    for (i = 0; i < 3U; ++i) {
        const uint32_t destination_index = receive_order[i];

        if (mem_service_provider_channel_wait_receive(
                &channel,
                &destination[destination_index],
                0,
                FIXTURE_BYTES,
                fixture->expected_checksum[destination_index],
                &completion) != 0 ||
            completion.transferred_bytes != FIXTURE_BYTES ||
            completion.checksum !=
                fixture->expected_checksum[destination_index]) {
            goto deregister;
        }
        fixture->receive_fence_count += 1U;
    }
    pthread_mutex_lock(&fixture->lock);
    while (!fixture->client_done) {
        pthread_cond_wait(&fixture->condition, &fixture->lock);
    }
    pthread_mutex_unlock(&fixture->lock);
    rc = 0;

deregister:
    for (i = 0; i < 2U; ++i) {
        if (destination[i].registered &&
            mem_service_provider_channel_deregister_region(
                &channel, &destination[i]) != 0) {
            rc = -1;
        }
    }

done:
    pthread_mutex_lock(&fixture->lock);
    fixture->server_rc = rc;
    fixture->remote_ready = true;
    pthread_cond_broadcast(&fixture->condition);
    pthread_mutex_unlock(&fixture->lock);
    mem_service_provider_tcp_endpoint_close(&endpoint);
    return NULL;
}

static void fill_payload(uint8_t *payload, size_t len, uint8_t seed)
{
    size_t i;

    for (i = 0; i < len; ++i) {
        payload[i] = (uint8_t)((i * 29U + seed) & 0xffU);
    }
}

int main(int argc, char **argv)
{
    struct fixture_state fixture;
    struct mem_service_provider_tcp_config config;
    struct mem_service_provider_tcp_endpoint endpoint = {0};
    struct mem_service_provider_tcp_canary_result canary;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_channel channel;
    struct mem_service_provider_region_binding source[2];
    struct mem_service_region_request request;
    struct mem_service_transfer_completion completion;
    enum mem_service_provider_state state;
    pthread_t server_thread;
    uint8_t payload[2][FIXTURE_BYTES];
    uint64_t checksum[2];
    uint64_t first_completion_id;
    uint64_t second_completion_id;
    uint32_t i;
    int rc = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s PORT\n", argv[0]);
        return 2;
    }
    memset(&fixture, 0, sizeof(fixture));
    fixture.port = (uint16_t)strtoul(argv[1], NULL, 10);
    fixture.server_rc = -1;
    fill_payload(payload[0], sizeof(payload[0]), 7U);
    fill_payload(payload[1], sizeof(payload[1]), 113U);
    for (i = 0; i < 2U; ++i) {
        fixture.expected_checksum[i] =
            mem_service_provider_checksum64(
                payload[i], sizeof(payload[i]));
    }
    if (fixture.port == 0 ||
        fixture.expected_checksum[0] == 0 ||
        fixture.expected_checksum[1] == 0 ||
        pthread_mutex_init(&fixture.lock, NULL) != 0 ||
        pthread_cond_init(&fixture.condition, NULL) != 0 ||
        pthread_create(&server_thread, NULL, run_server, &fixture) != 0) {
        return 2;
    }
    usleep(100000);
    memset(&config, 0, sizeof(config));
    config.local_ipv4 = "127.0.0.1";
    config.peer_ipv4 = "127.0.0.1";
    config.port = fixture.port;
    config.timeout_ms = 10000;
    memset(&source, 0, sizeof(source));
    if (mem_service_provider_tcp_endpoint_open(
            &endpoint, &config, false) != 0 ||
        mem_service_provider_tcp_endpoint_verify(
            &endpoint, false, FIXTURE_BYTES, 1, &canary) != 0 ||
        !canary.data_plane_ready ||
        bind_endpoint(&endpoint, &registry, &channel) != 0) {
        goto client_done;
    }
    pthread_mutex_lock(&fixture.lock);
    while (!fixture.remote_ready) {
        pthread_cond_wait(&fixture.condition, &fixture.lock);
    }
    pthread_mutex_unlock(&fixture.lock);
    if (fixture.server_rc != -1 ||
        strcmp(fixture.remote[0].provider_name, "tcp") != 0 ||
        strcmp(fixture.remote[1].provider_name, "tcp") != 0 ||
        fixture.remote[0].len != FIXTURE_BYTES ||
        fixture.remote[1].len != FIXTURE_BYTES) {
        goto client_done;
    }
    for (i = 0; i < 2U; ++i) {
        memset(&request, 0, sizeof(request));
        request.base = payload[i];
        request.len = sizeof(payload[i]);
        request.memory_kind = MEM_SERVICE_MEMORY_HOST;
        if (mem_service_provider_channel_register_region(
                &channel, &request, &source[i]) != 0) {
            goto client_done;
        }
    }
    checksum[0] =
        mem_service_provider_checksum64(payload[0], sizeof(payload[0]));
    checksum[1] =
        mem_service_provider_checksum64(payload[1], sizeof(payload[1]));
    if (mem_service_provider_channel_transfer(
            &channel,
            &source[0],
            0,
            &fixture.remote[0],
            0,
            sizeof(payload[0]),
            checksum[0],
            &completion) != 0 ||
        completion.transferred_bytes != sizeof(payload[0]) ||
        completion.checksum != checksum[0] ||
        memcmp(payload[0],
               fixture.destination[0],
               sizeof(payload[0])) != 0) {
        goto client_done;
    }
    if (mem_service_provider_channel_submit_transfer(
            &channel,
            &source[0],
            0,
            &fixture.remote[0],
            0,
            sizeof(payload[0]),
            checksum[0],
            &first_completion_id) != 0 ||
        mem_service_provider_channel_submit_transfer(
            &channel,
            &source[1],
            0,
            &fixture.remote[1],
            0,
            sizeof(payload[1]),
            checksum[1],
            &second_completion_id) != 0 ||
        first_completion_id == second_completion_id ||
        mem_service_provider_channel_poll_transfer(
            &channel,
            second_completion_id,
            sizeof(payload[1]),
            checksum[1],
            &completion) != 0 ||
        completion.id != second_completion_id ||
        mem_service_provider_channel_poll_transfer(
            &channel,
            first_completion_id,
            sizeof(payload[0]),
            checksum[0],
            &completion) != 0 ||
        completion.id != first_completion_id ||
        memcmp(payload[1],
               fixture.destination[1],
               sizeof(payload[1])) != 0) {
        goto client_done;
    }
    if (mem_service_provider_channel_transfer(
            &channel,
            &source[0],
            0,
            &fixture.remote[0],
            fixture.remote[0].len - 1U,
            sizeof(payload[0]),
            checksum[0],
            &completion) == 0) {
        goto client_done;
    }
    if (mem_service_provider_channel_transfer(
            &channel,
            &source[0],
            0,
            &fixture.remote[0],
            0,
            sizeof(payload[0]),
            checksum[0] ^ 1U,
            &completion) == 0 ||
        mem_service_provider_registry_refresh(&registry) != 0 ||
        mem_service_provider_registry_data_plane_ready(&registry) ||
        registry.providers[0].ops->probe(
            registry.providers[0].context, &state) != 0 ||
        state != MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE) {
        goto client_done;
    }
    rc = 0;

client_done:
    for (i = 0; i < 2U; ++i) {
        if (source[i].registered) {
            (void)mem_service_provider_channel_deregister_region(
                &channel, &source[i]);
        }
    }
    pthread_mutex_lock(&fixture.lock);
    fixture.client_done = true;
    pthread_cond_broadcast(&fixture.condition);
    pthread_mutex_unlock(&fixture.lock);
    mem_service_provider_tcp_endpoint_close(&endpoint);
    pthread_join(server_thread, NULL);
    pthread_cond_destroy(&fixture.condition);
    pthread_mutex_destroy(&fixture.lock);
    if (rc != 0 || fixture.server_rc != 0 ||
        fixture.receive_fence_count != 3U) {
        return 1;
    }
    printf("mem_service tcp-provider-conformance: status=ok "
           "registration=verified bounds=fail-closed "
           "transfer=verified completion=split+out-of-order "
           "receive_fence=demuxed receive_mode=wait "
           "checksum=fail-closed\n");
    return 0;
}
