#include "mem_service_provider_tcp.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *option_value(int argc, char **argv, const char *name)
{
    int i;

    for (i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int parse_u64(const char *text,
                     uint64_t min,
                     uint64_t max,
                     uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || text[0] == '\0' || value_out == NULL) {
        return -1;
    }
    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value < min || value > max) {
        return -1;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s protocol-fixtures\n"
            "  %s server-canary --local-ip IP --peer-ip IP --port PORT "
            "[--bytes N] [--iterations N] [--timeout-ms N]\n"
            "  %s client-canary --local-ip IP --peer-ip IP --port PORT "
            "[--bytes N] [--iterations N] [--timeout-ms N]\n",
            program,
            program,
            program);
}

static int run_canary(int argc, char **argv, bool server)
{
    const char *local_ipv4 = option_value(argc, argv, "--local-ip");
    const char *peer_ipv4 = option_value(argc, argv, "--peer-ip");
    const char *port_text = option_value(argc, argv, "--port");
    const char *bytes_text = option_value(argc, argv, "--bytes");
    const char *iterations_text = option_value(argc, argv, "--iterations");
    const char *timeout_text = option_value(argc, argv, "--timeout-ms");
    uint64_t port;
    uint64_t payload_bytes = 1024U * 1024U;
    uint64_t iterations = 10;
    uint64_t timeout_ms = 10000;
    struct mem_service_provider_tcp_config config;
    struct mem_service_provider_tcp_canary_result result;
    double seconds;
    double gib_per_second;
    int rc;

    if (local_ipv4 == NULL || peer_ipv4 == NULL ||
        parse_u64(port_text, 1, 65535, &port) != 0 ||
        (bytes_text != NULL &&
         parse_u64(bytes_text, 1, UINT32_MAX, &payload_bytes) != 0) ||
        (iterations_text != NULL &&
         parse_u64(iterations_text, 1, UINT32_MAX, &iterations) != 0) ||
        (timeout_text != NULL &&
         parse_u64(timeout_text, 1, UINT32_MAX, &timeout_ms) != 0)) {
        print_usage(argv[0]);
        return 2;
    }
    memset(&config, 0, sizeof(config));
    config.local_ipv4 = local_ipv4;
    config.peer_ipv4 = peer_ipv4;
    config.port = (uint16_t)port;
    config.timeout_ms = (uint32_t)timeout_ms;
    memset(&result, 0, sizeof(result));
    rc = server
             ? mem_service_provider_tcp_run_server_canary(
                   &config,
                   payload_bytes,
                   (uint32_t)iterations,
                   &result)
             : mem_service_provider_tcp_run_client_canary(
                   &config,
                   payload_bytes,
                   (uint32_t)iterations,
                   &result);
    if (rc != 0) {
        fprintf(stderr,
                "mem_service tcp-provider: role=%s status=failed rc=%d "
                "local=%s peer=%s; verify the matching peer command and "
                "pairwise rail\n",
                server ? "server" : "client",
                rc,
                local_ipv4,
                peer_ipv4);
        return 1;
    }
    seconds = (double)result.elapsed_us / 1000000.0;
    gib_per_second =
        seconds > 0
            ? ((double)result.payload_bytes * (double)result.iterations) /
                  (1024.0 * 1024.0 * 1024.0 * seconds)
            : 0.0;
    printf("mem_service tcp-provider: role=%s status=ok "
           "data_plane_ready=%u local=%s peer=%s "
           "bytes=%" PRIu64 " iterations=%" PRIu64
           " elapsed_us=%" PRIu64 " throughput_gib_s=%.3f "
           "checksum=0x%016" PRIx64 "\n",
           server ? "server" : "client",
           result.data_plane_ready ? 1U : 0U,
           result.local_ipv4,
           result.peer_ipv4,
           result.payload_bytes,
           result.iterations,
           result.elapsed_us,
           gib_per_second,
           result.checksum);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "protocol-fixtures") == 0) {
        return mem_service_provider_tcp_run_protocol_fixture();
    }
    if (strcmp(argv[1], "server-canary") == 0) {
        return run_canary(argc, argv, true);
    }
    if (strcmp(argv[1], "client-canary") == 0) {
        return run_canary(argc, argv, false);
    }
    print_usage(argv[0]);
    return 2;
}
