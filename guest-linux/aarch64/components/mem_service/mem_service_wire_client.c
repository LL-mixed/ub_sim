#include "mem_service_wire_client.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MEM_SERVICE_UNIX_SPEC_PREFIX "unix:"

const char *mem_service_wire_status_name(enum mem_service_wire_status status)
{
    switch (status) {
    case MEM_SERVICE_WIRE_STATUS_OK:
        return "ok";
    case MEM_SERVICE_WIRE_STATUS_NOT_FOUND:
        return "not_found";
    case MEM_SERVICE_WIRE_STATUS_STALE_REF:
        return "stale_ref";
    case MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH:
        return "checksum_mismatch";
    case MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT:
        return "version_conflict";
    case MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING:
        return "invalid_model_binding";
    case MEM_SERVICE_WIRE_STATUS_INVALID_SESSION:
        return "invalid_session";
    case MEM_SERVICE_WIRE_STATUS_TIMEOUT:
        return "timeout";
    case MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED:
        return "capacity_exceeded";
    case MEM_SERVICE_WIRE_STATUS_UNSUPPORTED:
        return "unsupported";
    case MEM_SERVICE_WIRE_STATUS_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

const char *mem_service_default_unix_socket_spec(void)
{
    return "unix:" MEM_SERVICE_DEFAULT_UNIX_SOCKET;
}

void mem_service_wire_client_options_init(
    struct mem_service_wire_client_options *options)
{
    if (options != NULL) {
        memset(options, 0, sizeof(*options));
        options->max_attempts = MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS;
    }
}

static uint32_t mem_service_client_effective_max_attempts(
    const struct mem_service_wire_client_options *options)
{
    uint32_t attempts;

    if (options == NULL || options->max_attempts == 0) {
        return MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS;
    }
    attempts = options->max_attempts;
    if (attempts > MEM_SERVICE_WIRE_CLIENT_MAX_ATTEMPTS) {
        attempts = MEM_SERVICE_WIRE_CLIENT_MAX_ATTEMPTS;
    }
    return attempts;
}

static void mem_service_client_sleep_ms(uint64_t delay_ms)
{
    struct timespec req;

    if (delay_ms == 0) {
        return;
    }
    req.tv_sec = (time_t)(delay_ms / 1000U);
    req.tv_nsec = (long)((delay_ms % 1000U) * 1000000U);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {
    }
}

static const char *mem_service_client_unix_path_from_spec(const char *spec)
{
    if (spec == NULL || spec[0] == '\0') {
        return NULL;
    }
    if (strncmp(spec, MEM_SERVICE_UNIX_SPEC_PREFIX,
                strlen(MEM_SERVICE_UNIX_SPEC_PREFIX)) == 0) {
        spec += strlen(MEM_SERVICE_UNIX_SPEC_PREFIX);
    }
    return spec[0] == '\0' ? NULL : spec;
}

static uint64_t mem_service_client_wall_clock_ms(void)
{
    time_t now = time(NULL);

    if (now < 0) {
        return 0;
    }
    return (uint64_t)now * 1000U;
}

static uint64_t mem_service_client_request_id(void)
{
    return mem_service_client_wall_clock_ms();
}

static int mem_service_client_read_full(int fd, void *buf, size_t len)
{
    uint8_t *cursor = (uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = read(fd, cursor + done, len - done);

        if (rc == 0) {
            return -1;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                return -2;
            }
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int mem_service_client_write_full(int fd, const void *buf, size_t len)
{
    const uint8_t *cursor = (const uint8_t *)buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = write(fd, cursor + done, len - done);

        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                return -2;
            }
            return -1;
        }
        if (rc == 0) {
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int mem_service_client_configure_timeout(
    int fd,
    const struct mem_service_wire_client_options *options)
{
    struct timeval timeout;
    uint64_t timeout_ms;

    if (options == NULL || options->timeout_ms == 0) {
        return 0;
    }
    timeout_ms = options->timeout_ms;
    timeout.tv_sec = (time_t)(timeout_ms / 1000U);
    timeout.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
    if (timeout.tv_sec == 0 && timeout.tv_usec == 0) {
        timeout.tv_usec = 1000;
    }
    if (setsockopt(fd,
                   SOL_SOCKET,
                   SO_RCVTIMEO,
                   &timeout,
                   sizeof(timeout)) != 0 ||
        setsockopt(fd,
                   SOL_SOCKET,
                   SO_SNDTIMEO,
                   &timeout,
                   sizeof(timeout)) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_client_prepare_unix_addr(const char *path,
                                                struct sockaddr_un *addr)
{
    size_t path_len;

    if (path == NULL) {
        return -1;
    }
    path_len = strlen(path);
    if (path_len == 0 || path_len >= sizeof(addr->sun_path)) {
        return -1;
    }
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    memcpy(addr->sun_path, path, path_len + 1);
    return 0;
}

static int mem_service_send_unix_request_once(
    const char *connect_spec,
    const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation,
    const char *payload_in,
    char *payload_out,
    size_t payload_out_len,
    enum mem_service_wire_status *status_out,
    bool emit_errors,
    bool *request_delivered_out,
    bool *timed_out_out)
{
    struct sockaddr_un addr;
    struct mem_service_wire_header request;
    struct mem_service_wire_header response;
    const char *path = mem_service_client_unix_path_from_spec(connect_spec);
    uint8_t payload[MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN];
    uint32_t payload_len = payload_in ? (uint32_t)strlen(payload_in) : 0;
    uint32_t payload_checksum = mem_service_wire_checksum(payload_in, payload_len);
    int fd;
    enum mem_service_wire_status status;
    int io_rc;

    if (request_delivered_out != NULL) {
        *request_delivered_out = false;
    }
    if (timed_out_out != NULL) {
        *timed_out_out = false;
    }
    if (status_out != NULL) {
        *status_out = MEM_SERVICE_WIRE_STATUS_INTERNAL;
    }

    if (payload_len > MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        if (emit_errors) {
            fprintf(stderr, "mem_service client: request payload too large\n");
        }
        if (status_out != NULL) {
            *status_out = MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED;
        }
        return 2;
    }
    if (path == NULL && connect_spec == NULL) {
        path = MEM_SERVICE_DEFAULT_UNIX_SOCKET;
    }
    if (mem_service_client_prepare_unix_addr(path, &addr) != 0) {
        if (emit_errors) {
            fprintf(stderr, "mem_service client: invalid unix connect path\n");
        }
        return 2;
    }
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        if (emit_errors) {
            perror("mem_service client: socket");
        }
        return 1;
    }
    if (mem_service_client_configure_timeout(fd, options) != 0) {
        if (emit_errors) {
            perror("mem_service client: timeout");
        }
        close(fd);
        return 1;
    }
    if (connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (emit_errors) {
            perror("mem_service client: connect");
        }
        close(fd);
        return 1;
    }
    mem_service_wire_init_header(&request,
                                 mem_service_client_request_id(),
                                 operation,
                                 payload_len,
                                 payload_checksum);
    io_rc = mem_service_client_write_full(fd, &request, sizeof(request));
    if (io_rc == 0 && payload_len > 0) {
        io_rc = mem_service_client_write_full(fd, payload_in, payload_len);
    }
    if (io_rc == 0 && request_delivered_out != NULL) {
        *request_delivered_out = true;
    }
    if (io_rc == 0) {
        io_rc = mem_service_client_read_full(fd, &response, sizeof(response));
    }
    if (io_rc != 0) {
        if (io_rc == -2) {
            if (emit_errors) {
                fprintf(stderr, "mem_service client: wire io timeout\n");
            }
            if (status_out != NULL) {
                *status_out = MEM_SERVICE_WIRE_STATUS_TIMEOUT;
            }
            if (timed_out_out != NULL) {
                *timed_out_out = true;
            }
            close(fd);
            return 1;
        }
        if (emit_errors) {
            perror("mem_service client: wire io");
        }
        close(fd);
        return 1;
    }
    if (!mem_service_wire_header_is_compatible(&response) ||
        response.request_id != request.request_id ||
        response.operation != request.operation ||
        response.payload_len > MEM_SERVICE_WIRE_MAX_PAYLOAD_LEN) {
        if (emit_errors) {
            fprintf(stderr, "mem_service client: invalid response\n");
        }
        close(fd);
        return 1;
    }
    if (response.payload_len > 0) {
        io_rc = mem_service_client_read_full(fd, payload, response.payload_len);
        if (io_rc != 0) {
            if (io_rc == -2) {
                if (emit_errors) {
                    fprintf(stderr, "mem_service client: response payload timeout\n");
                }
                if (status_out != NULL) {
                    *status_out = MEM_SERVICE_WIRE_STATUS_TIMEOUT;
                }
                if (timed_out_out != NULL) {
                    *timed_out_out = true;
                }
                close(fd);
                return 1;
            }
            if (emit_errors) {
                fprintf(stderr, "mem_service client: short response payload\n");
            }
            close(fd);
            return 1;
        }
        if (mem_service_wire_checksum(payload, response.payload_len) !=
            response.payload_checksum) {
            if (emit_errors) {
                fprintf(stderr, "mem_service client: response checksum mismatch\n");
            }
            close(fd);
            return 1;
        }
    }
    status = (enum mem_service_wire_status)response.status;
    if (status_out != NULL) {
        *status_out = status;
    }
    if (payload_out != NULL && payload_out_len > 0) {
        size_t copy_len = response.payload_len;

        if (copy_len >= payload_out_len) {
            copy_len = payload_out_len - 1;
        }
        if (copy_len > 0) {
            memcpy(payload_out, payload, copy_len);
        }
        payload_out[copy_len] = '\0';
    }
    close(fd);
    return status == MEM_SERVICE_WIRE_STATUS_OK ? 0 : 1;
}

static bool mem_service_client_should_retry(
    const struct mem_service_wire_client_options *options,
    uint32_t attempt_index,
    uint32_t max_attempts,
    int rc,
    bool request_delivered,
    bool timed_out)
{
    if (rc == 0 || attempt_index + 1U >= max_attempts) {
        return false;
    }
    if (!request_delivered) {
        return true;
    }
    return timed_out && options != NULL && options->retry_on_timeout != 0;
}

int mem_service_send_unix_request_with_options(
    const char *connect_spec,
    const struct mem_service_wire_client_options *options,
    enum mem_service_wire_operation operation,
    const char *payload_in,
    char *payload_out,
    size_t payload_out_len,
    enum mem_service_wire_status *status_out)
{
    uint32_t max_attempts = mem_service_client_effective_max_attempts(options);
    uint32_t attempt_index;
    int rc = 1;

    for (attempt_index = 0; attempt_index < max_attempts; ++attempt_index) {
        bool request_delivered = false;
        bool timed_out = false;
        bool last_attempt = attempt_index + 1U == max_attempts;

        rc = mem_service_send_unix_request_once(connect_spec,
                                                options,
                                                operation,
                                                payload_in,
                                                payload_out,
                                                payload_out_len,
                                                status_out,
                                                last_attempt,
                                                &request_delivered,
                                                &timed_out);
        if (!mem_service_client_should_retry(options,
                                             attempt_index,
                                             max_attempts,
                                             rc,
                                             request_delivered,
                                             timed_out)) {
            break;
        }
        if (options != NULL) {
            mem_service_client_sleep_ms(options->retry_backoff_ms);
        }
    }
    return rc;
}

int mem_service_send_unix_request(const char *connect_spec,
                                  enum mem_service_wire_operation operation,
                                  const char *payload_in,
                                  char *payload_out,
                                  size_t payload_out_len,
                                  enum mem_service_wire_status *status_out)
{
    return mem_service_send_unix_request_with_options(connect_spec,
                                                      NULL,
                                                      operation,
                                                      payload_in,
                                                      payload_out,
                                                      payload_out_len,
                                                      status_out);
}
