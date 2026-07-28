#ifndef MEM_SERVICE_PROVIDER_ROCE_H
#define MEM_SERVICE_PROVIDER_ROCE_H

#include "mem_service_provider.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct mem_service_provider_roce_config {
    const char *local_ipv4;
    const char *peer_ipv4;
    const char *expected_device;
    uint16_t port;
    uint32_t timeout_ms;
};

struct mem_service_provider_roce_canary_result {
    char device[64];
    char local_ipv4[48];
    char peer_ipv4[48];
    uint64_t payload_bytes;
    uint64_t iterations;
    uint64_t checksum;
    uint64_t elapsed_us;
    bool data_plane_ready;
};

struct mem_service_provider_roce_endpoint {
    void *implementation;
};

int mem_service_provider_roce_probe_device(const char *device,
                                           char *detail,
                                           size_t detail_len);
int mem_service_provider_roce_endpoint_open(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config,
    bool server);
int mem_service_provider_roce_endpoint_listen(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config);
int mem_service_provider_roce_endpoint_accept(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config);
int mem_service_provider_roce_endpoint_verify(
    struct mem_service_provider_roce_endpoint *endpoint,
    bool server,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result);
int mem_service_provider_roce_endpoint_registration(
    struct mem_service_provider_roce_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out);
void mem_service_provider_roce_endpoint_close(
    struct mem_service_provider_roce_endpoint *endpoint);
int mem_service_provider_roce_run_server_canary(
    const struct mem_service_provider_roce_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result);
int mem_service_provider_roce_run_client_canary(
    const struct mem_service_provider_roce_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result);
int mem_service_provider_roce_run_protocol_fixture(void);

#endif
