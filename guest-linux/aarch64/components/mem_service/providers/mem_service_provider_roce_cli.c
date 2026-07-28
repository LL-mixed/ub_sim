#include "mem_service_provider_roce.h"
#include "../mem_service_daemon.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEM_SERVICE_ROCE_MESH_MAX_ENDPOINTS 8U

struct mem_service_roce_mesh_endpoint_config {
    bool server;
    char local_ipv4[48];
    char peer_ipv4[48];
    char device[64];
    uint16_t port;
};

struct mem_service_roce_mesh_config {
    char listen[160];
    char store[512];
    char storage_root[512];
    char metrics_listen[160];
    uint64_t verify_bytes;
    uint32_t verify_iterations;
    uint32_t timeout_ms;
    size_t endpoint_count;
    struct mem_service_roce_mesh_endpoint_config
        endpoints[MEM_SERVICE_ROCE_MESH_MAX_ENDPOINTS];
};

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s probe --device DEVICE\n"
            "  %s protocol-fixtures\n"
            "  %s mesh-serve --config PATH\n"
            "  %s server-canary --local-ip IPV4 --peer-ip IPV4 --port PORT "
            "--device DEVICE [--bytes N] [--iterations N] "
            "[--timeout-ms N]\n"
            "  %s client-canary --local-ip IPV4 --peer-ip IPV4 --port PORT "
            "--device DEVICE [--bytes N] [--iterations N] "
            "[--timeout-ms N]\n",
            program,
            program,
            program,
            program,
            program);
}

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

static int parse_u64(const char *value,
                     uint64_t minimum,
                     uint64_t maximum,
                     uint64_t *parsed_out)
{
    char *end = NULL;
    unsigned long long parsed;

    if (value == NULL || value[0] == '\0' || value[0] == '-' ||
        parsed_out == NULL) {
        return -1;
    }
    errno = 0;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' ||
        parsed < minimum || parsed > maximum) {
        return -1;
    }
    *parsed_out = (uint64_t)parsed;
    return 0;
}

static void trim_ascii(char *value)
{
    char *start = value;
    size_t len;

    while (*start != '\0' && isspace((unsigned char)*start)) {
        start += 1;
    }
    if (start != value) {
        memmove(value, start, strlen(start) + 1U);
    }
    len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1U])) {
        value[len - 1U] = '\0';
        len -= 1U;
    }
}

static int copy_config_value(char *destination,
                             size_t destination_len,
                             const char *value)
{
    size_t len;

    if (destination == NULL || destination_len == 0 ||
        value == NULL || value[0] == '\0') {
        return -1;
    }
    len = strlen(value);
    if (len >= destination_len) {
        return -1;
    }
    memcpy(destination, value, len + 1U);
    return 0;
}

static int parse_mesh_endpoint(
    const char *value,
    struct mem_service_roce_mesh_endpoint_config *endpoint)
{
    char copy[256];
    char *fields[5];
    char *cursor;
    uint64_t port;
    size_t i;

    if (value == NULL || endpoint == NULL ||
        copy_config_value(copy, sizeof(copy), value) != 0) {
        return -1;
    }
    cursor = copy;
    for (i = 0; i < 5U; ++i) {
        char *separator;

        fields[i] = cursor;
        separator = strchr(cursor, ',');
        if (i == 4U) {
            if (separator != NULL) {
                return -1;
            }
        } else {
            if (separator == NULL) {
                return -1;
            }
            *separator = '\0';
            cursor = separator + 1U;
        }
        trim_ascii(fields[i]);
        if (fields[i][0] == '\0') {
            return -1;
        }
    }
    memset(endpoint, 0, sizeof(*endpoint));
    if (strcmp(fields[0], "server") == 0) {
        endpoint->server = true;
    } else if (strcmp(fields[0], "client") != 0) {
        return -1;
    }
    if (copy_config_value(endpoint->local_ipv4,
                          sizeof(endpoint->local_ipv4),
                          fields[1]) != 0 ||
        copy_config_value(endpoint->peer_ipv4,
                          sizeof(endpoint->peer_ipv4),
                          fields[2]) != 0 ||
        parse_u64(fields[3], 1, 65535, &port) != 0 ||
        copy_config_value(endpoint->device,
                          sizeof(endpoint->device),
                          fields[4]) != 0) {
        return -1;
    }
    endpoint->port = (uint16_t)port;
    return 0;
}

static int load_mesh_config(
    const char *path,
    struct mem_service_roce_mesh_config *config)
{
    FILE *file;
    char line[1024];
    bool version_seen = false;
    bool listen_seen = false;
    bool store_seen = false;
    bool storage_root_seen = false;
    bool metrics_listen_seen = false;
    bool verify_bytes_seen = false;
    bool verify_iterations_seen = false;
    bool timeout_seen = false;

    if (path == NULL || path[0] == '\0' || config == NULL ||
        (file = fopen(path, "r")) == NULL) {
        return -1;
    }
    memset(config, 0, sizeof(*config));
    config->verify_bytes = 64U * 1024U;
    config->verify_iterations = 1U;
    config->timeout_ms = 30000U;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *separator;
        char *name;
        char *value;
        uint64_t parsed;

        if (strchr(line, '\n') == NULL && !feof(file)) {
            fclose(file);
            return -1;
        }
        trim_ascii(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        separator = strchr(line, '=');
        if (separator == NULL) {
            fclose(file);
            return -1;
        }
        *separator = '\0';
        name = line;
        value = separator + 1U;
        trim_ascii(name);
        trim_ascii(value);
        if (strcmp(name, "version") == 0) {
            if (version_seen || strcmp(value, "1") != 0) {
                fclose(file);
                return -1;
            }
            version_seen = true;
        } else if (strcmp(name, "listen") == 0) {
            if (listen_seen || strncmp(value, "unix:", 5) != 0 ||
                copy_config_value(config->listen,
                                  sizeof(config->listen),
                                  value) != 0) {
                fclose(file);
                return -1;
            }
            listen_seen = true;
        } else if (strcmp(name, "store") == 0) {
            if (store_seen ||
                copy_config_value(config->store,
                                  sizeof(config->store),
                                  value) != 0) {
                fclose(file);
                return -1;
            }
            store_seen = true;
        } else if (strcmp(name, "storage_root") == 0) {
            if (storage_root_seen ||
                copy_config_value(config->storage_root,
                                  sizeof(config->storage_root),
                                  value) != 0) {
                fclose(file);
                return -1;
            }
            storage_root_seen = true;
        } else if (strcmp(name, "metrics_listen") == 0) {
            if (metrics_listen_seen ||
                strncmp(value, "tcp:127.0.0.1:", 14) != 0 ||
                copy_config_value(config->metrics_listen,
                                  sizeof(config->metrics_listen),
                                  value) != 0) {
                fclose(file);
                return -1;
            }
            metrics_listen_seen = true;
        } else if (strcmp(name, "verify_bytes") == 0) {
            if (verify_bytes_seen ||
                parse_u64(value, 1, UINT32_MAX, &parsed) != 0) {
                fclose(file);
                return -1;
            }
            config->verify_bytes = parsed;
            verify_bytes_seen = true;
        } else if (strcmp(name, "verify_iterations") == 0) {
            if (verify_iterations_seen ||
                parse_u64(value, 1, UINT32_MAX, &parsed) != 0) {
                fclose(file);
                return -1;
            }
            config->verify_iterations = (uint32_t)parsed;
            verify_iterations_seen = true;
        } else if (strcmp(name, "timeout_ms") == 0) {
            if (timeout_seen ||
                parse_u64(value, 1, UINT32_MAX, &parsed) != 0) {
                fclose(file);
                return -1;
            }
            config->timeout_ms = (uint32_t)parsed;
            timeout_seen = true;
        } else if (strcmp(name, "endpoint") == 0) {
            if (config->endpoint_count >=
                    MEM_SERVICE_ROCE_MESH_MAX_ENDPOINTS ||
                parse_mesh_endpoint(
                    value,
                    &config->endpoints[config->endpoint_count]) != 0) {
                fclose(file);
                return -1;
            }
            config->endpoint_count += 1U;
        } else {
            fclose(file);
            return -1;
        }
    }
    if (ferror(file) != 0) {
        fclose(file);
        return -1;
    }
    fclose(file);
    return version_seen && listen_seen && config->endpoint_count > 0
               ? 0
               : -1;
}

static int run_probe(int argc, char **argv)
{
    const char *device = option_value(argc, argv, "--device");
    char detail[256];

    if (device == NULL ||
        mem_service_provider_roce_probe_device(device,
                                               detail,
                                               sizeof(detail)) != 0) {
        fprintf(stderr,
                "mem_service roce-provider: probe failed; check --device, "
                "port state, and Ethernet link layer\n");
        return 1;
    }
    printf("mem_service roce-provider: status=available %s\n", detail);
    return 0;
}

static int run_mesh_serve(int argc, char **argv)
{
    const char *config_path = option_value(argc, argv, "--config");
    struct mem_service_roce_mesh_config config;
    struct mem_service_provider_roce_endpoint
        endpoints[MEM_SERVICE_ROCE_MESH_MAX_ENDPOINTS];
    struct mem_service_provider_registry providers;
    struct mem_service_daemon_runtime runtime;
    size_t opened = 0;
    size_t i;
    int rc = 1;

    if (config_path == NULL ||
        load_mesh_config(config_path, &config) != 0 ||
        mem_service_provider_registry_init(&providers) != 0) {
        fprintf(stderr,
                "mem_service roce-provider: invalid mesh config; "
                "check version, listen, and endpoint entries\n");
        return 2;
    }
    memset(endpoints, 0, sizeof(endpoints));
    for (i = 0; i < config.endpoint_count; ++i) {
        const struct mem_service_roce_mesh_endpoint_config *entry =
            &config.endpoints[i];
        struct mem_service_provider_roce_config endpoint_config = {
            .local_ipv4 = entry->local_ipv4,
            .peer_ipv4 = entry->peer_ipv4,
            .expected_device = entry->device,
            .port = entry->port,
            .timeout_ms = config.timeout_ms,
        };
        struct mem_service_provider_roce_canary_result result;
        struct mem_service_provider_registration registration;
        int verify_rc;

        if (mem_service_provider_roce_endpoint_open(&endpoints[i],
                                                    &endpoint_config,
                                                    entry->server) != 0) {
            fprintf(stderr,
                    "mem_service roce-provider: endpoint open failed "
                    "index=%zu role=%s local=%s peer=%s device=%s\n",
                    i,
                    entry->server ? "server" : "client",
                    entry->local_ipv4,
                    entry->peer_ipv4,
                    entry->device);
            goto done;
        }
        opened += 1U;
        verify_rc = mem_service_provider_roce_endpoint_verify(
            &endpoints[i],
            entry->server,
            config.verify_bytes,
            config.verify_iterations,
            &result);
        if (verify_rc != 0) {
            fprintf(stderr,
                    "mem_service roce-provider: endpoint failed stage=verify "
                    "verify_rc=%d index=%zu local=%s peer=%s device=%s\n",
                    verify_rc,
                    i,
                    entry->local_ipv4,
                    entry->peer_ipv4,
                    entry->device);
            goto done;
        }
        if (mem_service_provider_roce_endpoint_registration(
                &endpoints[i], &registration) != 0) {
            fprintf(stderr,
                    "mem_service roce-provider: endpoint failed "
                    "stage=registration index=%zu\n",
                    i);
            goto done;
        }
        if (mem_service_provider_registry_register(
                &providers, &registration) != 0) {
            fprintf(stderr,
                    "mem_service roce-provider: endpoint failed "
                    "stage=registry index=%zu\n",
                    i);
            goto done;
        }
        printf("mem_service roce-provider: mesh_endpoint=%zu status=ready "
               "device=%s local=%s peer=%s verify_bytes=%" PRIu64
               " verify_iterations=%u\n",
               i,
               result.device,
               result.local_ipv4,
               result.peer_ipv4,
               result.payload_bytes,
               (unsigned int)result.iterations);
        fflush(stdout);
    }
    if (mem_service_provider_registry_ready_count(&providers) !=
            providers.count ||
        !mem_service_provider_registry_data_plane_ready(&providers)) {
        fprintf(stderr,
                "mem_service roce-provider: mesh readiness failed "
                "providers=%zu ready=%zu\n",
                providers.count,
                mem_service_provider_registry_ready_count(&providers));
        goto done;
    }
    memset(&runtime, 0, sizeof(runtime));
    runtime.providers = &providers;
    rc = mem_service_run_unix_daemon_with_runtime(
        config.listen,
        config.store[0] != '\0' ? config.store : NULL,
        config.metrics_listen[0] != '\0' ? config.metrics_listen : NULL,
        config.storage_root[0] != '\0' ? config.storage_root : NULL,
        &runtime);

done:
    while (opened > 0) {
        opened -= 1U;
        mem_service_provider_roce_endpoint_close(&endpoints[opened]);
    }
    return rc;
}

static int run_canary(int argc, char **argv, int server)
{
    const char *local_ipv4 = option_value(argc, argv, "--local-ip");
    const char *peer_ipv4 = option_value(argc, argv, "--peer-ip");
    const char *device = option_value(argc, argv, "--device");
    const char *port_text = option_value(argc, argv, "--port");
    const char *bytes_text = option_value(argc, argv, "--bytes");
    const char *iterations_text = option_value(argc, argv, "--iterations");
    const char *timeout_text = option_value(argc, argv, "--timeout-ms");
    uint64_t port;
    uint64_t payload_bytes = 1024U * 1024U;
    uint64_t iterations = 10;
    uint64_t timeout_ms = 10000;
    struct mem_service_provider_roce_config config;
    struct mem_service_provider_roce_canary_result result;
    double seconds;
    double gib_per_second;
    int rc;

    if (local_ipv4 == NULL || peer_ipv4 == NULL || device == NULL ||
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
    config.expected_device = device;
    config.port = (uint16_t)port;
    config.timeout_ms = (uint32_t)timeout_ms;
    memset(&result, 0, sizeof(result));
    rc = server
             ? mem_service_provider_roce_run_server_canary(
                   &config,
                   payload_bytes,
                   (uint32_t)iterations,
                   &result)
             : mem_service_provider_roce_run_client_canary(
                   &config,
                   payload_bytes,
                   (uint32_t)iterations,
                   &result);
    if (rc != 0) {
        fprintf(stderr,
                "mem_service roce-provider: role=%s status=failed rc=%d "
                "local=%s peer=%s device=%s; verify the matching peer "
                "command and pairwise rail\n",
                server ? "server" : "client",
                rc,
                local_ipv4,
                peer_ipv4,
                device);
        return 1;
    }
    seconds = (double)result.elapsed_us / 1000000.0;
    gib_per_second =
        seconds > 0
            ? ((double)result.payload_bytes * (double)result.iterations) /
                  (1024.0 * 1024.0 * 1024.0 * seconds)
            : 0.0;
    printf("mem_service roce-provider: role=%s status=ok "
           "data_plane_ready=%u device=%s local=%s peer=%s "
           "bytes=%" PRIu64 " iterations=%" PRIu64
           " elapsed_us=%" PRIu64 " throughput_gib_s=%.3f "
           "checksum=0x%016" PRIx64 "\n",
           server ? "server" : "client",
           result.data_plane_ready ? 1U : 0U,
           result.device,
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
    if (strcmp(argv[1], "probe") == 0) {
        return run_probe(argc, argv);
    }
    if (strcmp(argv[1], "protocol-fixtures") == 0) {
        return mem_service_provider_roce_run_protocol_fixture();
    }
    if (strcmp(argv[1], "mesh-serve") == 0) {
        return run_mesh_serve(argc, argv);
    }
    if (strcmp(argv[1], "server-canary") == 0) {
        return run_canary(argc, argv, 1);
    }
    if (strcmp(argv[1], "client-canary") == 0) {
        return run_canary(argc, argv, 0);
    }
    print_usage(argv[0]);
    return 2;
}
