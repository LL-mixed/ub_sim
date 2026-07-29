#include "mem_service_provider_tcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MEM_SERVICE_TCP_DESCRIPTOR_MAGIC 0x4d535444U
#define MEM_SERVICE_TCP_MESSAGE_MAGIC 0x4d535454U
#define MEM_SERVICE_TCP_PROTOCOL_VERSION 1U
#define MEM_SERVICE_TCP_MAX_REGIONS 16U
#define MEM_SERVICE_TCP_MAX_COMPLETIONS 64U
#define MEM_SERVICE_TCP_SOCKET_BUFFER_BYTES (16U * 1024U * 1024U)

enum mem_service_tcp_message_type {
    MEM_SERVICE_TCP_MESSAGE_REGION = 1,
    MEM_SERVICE_TCP_MESSAGE_TRANSFER = 2,
    MEM_SERVICE_TCP_MESSAGE_ACK = 3,
};

struct mem_service_tcp_descriptor_wire {
    uint32_t magic;
    uint32_t version;
    uint64_t region_handle;
    uint64_t len;
};

struct mem_service_tcp_message_wire {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t status;
    uint64_t completion_id;
    uint64_t region_handle;
    uint64_t offset;
    uint64_t len;
    uint64_t checksum;
};

struct mem_service_tcp_message {
    uint32_t type;
    uint32_t status;
    uint64_t completion_id;
    uint64_t region_handle;
    uint64_t offset;
    uint64_t len;
    uint64_t checksum;
};

struct mem_service_tcp_region_slot {
    bool in_use;
    bool receive_ready;
    uint64_t handle;
    void *base;
    uint64_t len;
    uint64_t receive_offset;
    struct mem_service_transfer_completion receive_completion;
};

struct mem_service_tcp_pending_completion {
    struct mem_service_transfer_completion completion;
    uint64_t destination_handle;
    uint64_t destination_offset;
};

struct mem_service_tcp_context {
    int listen_fd;
    int socket_fd;
    uint32_t timeout_ms;
    char local_ipv4[48];
    char peer_ipv4[48];
    char instance[64];
    bool server;
    bool connected;
    bool transfer_verified;
    bool receive_on_wait;
    bool receiver_started;
    pthread_t receiver_thread;
    pthread_mutex_t state_lock;
    pthread_mutex_t region_lock;
    pthread_mutex_t io_lock;
    pthread_cond_t receive_condition;
    struct mem_service_tcp_region_slot regions[MEM_SERVICE_TCP_MAX_REGIONS];
    uint64_t next_region_handle;
    uint64_t next_completion_id;
    struct mem_service_tcp_pending_completion
        pending[MEM_SERVICE_TCP_MAX_COMPLETIONS];
};

static uint64_t mem_service_tcp_hton64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)value) << 32);
#else
    return value;
#endif
}

static uint64_t mem_service_tcp_ntoh64(uint64_t value)
{
    return mem_service_tcp_hton64(value);
}

static uint64_t mem_service_tcp_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static int mem_service_tcp_error(const char *stage)
{
    fprintf(stderr, "mem_service tcp-provider: stage=%s errno=%d (%s)\n",
            stage, errno, strerror(errno));
    return -1;
}

static int mem_service_tcp_fill_address(const char *ipv4,
                                        uint16_t port,
                                        struct sockaddr_in *address)
{
    if (ipv4 == NULL || ipv4[0] == '\0' || address == NULL) {
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    return inet_pton(AF_INET, ipv4, &address->sin_addr) == 1 ? 0 : -1;
}

static bool mem_service_tcp_state_connected(
    struct mem_service_tcp_context *context)
{
    bool connected;

    pthread_mutex_lock(&context->state_lock);
    connected = context->connected;
    pthread_mutex_unlock(&context->state_lock);
    return connected;
}

static void mem_service_tcp_set_state(struct mem_service_tcp_context *context,
                                      bool connected,
                                      bool transfer_verified)
{
    pthread_mutex_lock(&context->state_lock);
    context->connected = connected;
    context->transfer_verified = transfer_verified;
    pthread_mutex_unlock(&context->state_lock);
}

static void mem_service_tcp_fail_connection(
    struct mem_service_tcp_context *context)
{
    mem_service_tcp_set_state(context, false, false);
    pthread_mutex_lock(&context->region_lock);
    pthread_cond_broadcast(&context->receive_condition);
    pthread_mutex_unlock(&context->region_lock);
    if (context->socket_fd >= 0) {
        (void)shutdown(context->socket_fd, SHUT_RDWR);
    }
}

static int mem_service_tcp_configure_socket(int fd, uint32_t timeout_ms)
{
    int enabled = 1;
    int buffer_bytes = MEM_SERVICE_TCP_SOCKET_BUFFER_BYTES;
    struct timeval timeout;

    memset(&timeout, 0, sizeof(timeout));
    timeout.tv_sec = (time_t)(timeout_ms / 1000U);
    timeout.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                   &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                   &enabled, sizeof(enabled)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                   &timeout, sizeof(timeout)) != 0) {
        return -1;
    }
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                     &buffer_bytes, sizeof(buffer_bytes));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                     &buffer_bytes, sizeof(buffer_bytes));
    return 0;
}

static int mem_service_tcp_wait_fd(int fd, short events, uint32_t timeout_ms)
{
    struct pollfd descriptor;
    int rc;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = fd;
    descriptor.events = events;
    do {
        rc = poll(&descriptor, 1,
                  timeout_ms == 0 ? -1 : (int)timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0 ||
        (descriptor.revents & (POLLERR | POLLNVAL)) != 0 ||
        (descriptor.revents & events) == 0) {
        fprintf(stderr,
                "mem_service tcp-provider: stage=poll fd=%d events=0x%x "
                "rc=%d revents=0x%x timeout_ms=%u\n",
                fd,
                (unsigned int)events,
                rc,
                (unsigned int)descriptor.revents,
                timeout_ms);
        return -1;
    }
    return 0;
}

static int mem_service_tcp_send_all(int fd, const void *data, uint64_t len)
{
    const uint8_t *bytes = data;
    uint64_t sent = 0;

    while (sent < len) {
        size_t chunk = len - sent > SIZE_MAX ? SIZE_MAX : (size_t)(len - sent);
        ssize_t rc = send(fd, bytes + sent, chunk, MSG_NOSIGNAL);

        if (rc > 0) {
            sent += (uint64_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int mem_service_tcp_receive_all(int fd,
                                       void *data,
                                       uint64_t len,
                                       uint32_t timeout_ms)
{
    uint8_t *bytes = data;
    uint64_t received = 0;

    while (received < len) {
        size_t chunk =
            len - received > SIZE_MAX ? SIZE_MAX : (size_t)(len - received);
        ssize_t rc;

        if (mem_service_tcp_wait_fd(fd, POLLIN, timeout_ms) != 0) {
            return -1;
        }
        rc = recv(fd, bytes + received, chunk, 0);
        if (rc > 0) {
            received += (uint64_t)rc;
            continue;
        }
        if (rc < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
    return 0;
}

static int mem_service_tcp_send_message(
    struct mem_service_tcp_context *context,
    const struct mem_service_tcp_message *message)
{
    struct mem_service_tcp_message_wire wire;

    memset(&wire, 0, sizeof(wire));
    wire.magic = htonl(MEM_SERVICE_TCP_MESSAGE_MAGIC);
    wire.version = htonl(MEM_SERVICE_TCP_PROTOCOL_VERSION);
    wire.type = htonl(message->type);
    wire.status = htonl(message->status);
    wire.completion_id = mem_service_tcp_hton64(message->completion_id);
    wire.region_handle = mem_service_tcp_hton64(message->region_handle);
    wire.offset = mem_service_tcp_hton64(message->offset);
    wire.len = mem_service_tcp_hton64(message->len);
    wire.checksum = mem_service_tcp_hton64(message->checksum);
    return mem_service_tcp_send_all(context->socket_fd, &wire, sizeof(wire));
}

static int mem_service_tcp_receive_message(
    struct mem_service_tcp_context *context,
    struct mem_service_tcp_message *message,
    uint32_t timeout_ms)
{
    struct mem_service_tcp_message_wire wire;

    if (mem_service_tcp_receive_all(context->socket_fd,
                                    &wire,
                                    sizeof(wire),
                                    timeout_ms) != 0 ||
        ntohl(wire.magic) != MEM_SERVICE_TCP_MESSAGE_MAGIC ||
        ntohl(wire.version) != MEM_SERVICE_TCP_PROTOCOL_VERSION) {
        return -1;
    }
    memset(message, 0, sizeof(*message));
    message->type = ntohl(wire.type);
    message->status = ntohl(wire.status);
    message->completion_id = mem_service_tcp_ntoh64(wire.completion_id);
    message->region_handle = mem_service_tcp_ntoh64(wire.region_handle);
    message->offset = mem_service_tcp_ntoh64(wire.offset);
    message->len = mem_service_tcp_ntoh64(wire.len);
    message->checksum = mem_service_tcp_ntoh64(wire.checksum);
    return 0;
}

static struct mem_service_tcp_region_slot *mem_service_tcp_find_region(
    struct mem_service_tcp_context *context,
    uint64_t handle)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_TCP_MAX_REGIONS; ++i) {
        if (context->regions[i].in_use &&
            context->regions[i].handle == handle) {
            return &context->regions[i];
        }
    }
    return NULL;
}

static int mem_service_tcp_decode_descriptor(
    const struct mem_service_provider_descriptor *opaque,
    uint64_t *region_handle_out,
    uint64_t *len_out)
{
    struct mem_service_tcp_descriptor_wire descriptor;

    if (opaque == NULL || region_handle_out == NULL || len_out == NULL ||
        opaque->len != sizeof(descriptor)) {
        return -1;
    }
    memcpy(&descriptor, opaque->bytes, sizeof(descriptor));
    if (ntohl(descriptor.magic) != MEM_SERVICE_TCP_DESCRIPTOR_MAGIC ||
        ntohl(descriptor.version) != MEM_SERVICE_TCP_PROTOCOL_VERSION) {
        return -1;
    }
    *region_handle_out =
        mem_service_tcp_ntoh64(descriptor.region_handle);
    *len_out = mem_service_tcp_ntoh64(descriptor.len);
    return *region_handle_out == 0 || *len_out == 0 ? -1 : 0;
}

static int mem_service_tcp_provider_probe(
    void *opaque,
    enum mem_service_provider_state *state_out)
{
    struct mem_service_tcp_context *context = opaque;
    bool connected;
    bool verified;
    struct pollfd descriptor;

    if (context == NULL || state_out == NULL) {
        return -1;
    }
    pthread_mutex_lock(&context->state_lock);
    connected = context->connected;
    verified = context->transfer_verified;
    pthread_mutex_unlock(&context->state_lock);
    if (connected) {
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fd = context->socket_fd;
        descriptor.events = POLLIN;
        if (poll(&descriptor, 1, 0) < 0 ||
            (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            mem_service_tcp_fail_connection(context);
            connected = false;
            verified = false;
        }
    }
    *state_out = !connected
                     ? MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE
                     : verified ? MEM_SERVICE_PROVIDER_STATE_READY
                                : MEM_SERVICE_PROVIDER_STATE_DEGRADED;
    return 0;
}

static int mem_service_tcp_provider_register_region(
    void *opaque,
    const struct mem_service_region_request *request,
    struct mem_service_region *region_out)
{
    struct mem_service_tcp_context *context = opaque;
    struct mem_service_tcp_region_slot *slot = NULL;
    struct mem_service_tcp_descriptor_wire descriptor;
    size_t i;

    if (context == NULL || !mem_service_tcp_state_connected(context) ||
        request == NULL || region_out == NULL || request->base == NULL ||
        request->len == 0 || request->len > SIZE_MAX ||
        request->memory_kind != MEM_SERVICE_MEMORY_HOST) {
        return -1;
    }
    pthread_mutex_lock(&context->region_lock);
    for (i = 0; i < MEM_SERVICE_TCP_MAX_REGIONS; ++i) {
        if (!context->regions[i].in_use) {
            slot = &context->regions[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&context->region_lock);
        return -1;
    }
    context->next_region_handle += 1U;
    memset(slot, 0, sizeof(*slot));
    slot->in_use = true;
    slot->handle = context->next_region_handle;
    slot->base = request->base;
    slot->len = request->len;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = htonl(MEM_SERVICE_TCP_DESCRIPTOR_MAGIC);
    descriptor.version = htonl(MEM_SERVICE_TCP_PROTOCOL_VERSION);
    descriptor.region_handle = mem_service_tcp_hton64(slot->handle);
    descriptor.len = mem_service_tcp_hton64(slot->len);
    memset(region_out, 0, sizeof(*region_out));
    region_out->handle = slot->handle;
    region_out->len = slot->len;
    region_out->memory_kind = request->memory_kind;
    region_out->descriptor.len = sizeof(descriptor);
    memcpy(region_out->descriptor.bytes, &descriptor, sizeof(descriptor));
    pthread_mutex_unlock(&context->region_lock);
    return 0;
}

static int mem_service_tcp_provider_deregister_region(
    void *opaque,
    uint64_t region_handle)
{
    struct mem_service_tcp_context *context = opaque;
    struct mem_service_tcp_region_slot *slot;

    if (context == NULL) {
        return -1;
    }
    pthread_mutex_lock(&context->region_lock);
    slot = mem_service_tcp_find_region(context, region_handle);
    if (slot == NULL) {
        pthread_mutex_unlock(&context->region_lock);
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    pthread_mutex_unlock(&context->region_lock);
    return 0;
}

static int mem_service_tcp_receive_deadline(
    uint32_t timeout_ms,
    struct timespec *deadline)
{
    uint64_t nanoseconds;

    if (deadline == NULL ||
        clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return -1;
    }
    deadline->tv_sec += (time_t)(timeout_ms / 1000U);
    nanoseconds = (uint64_t)deadline->tv_nsec +
                  (uint64_t)(timeout_ms % 1000U) * 1000000ULL;
    deadline->tv_sec += (time_t)(nanoseconds / 1000000000ULL);
    deadline->tv_nsec = (long)(nanoseconds % 1000000000ULL);
    return 0;
}

static int mem_service_tcp_receive_transfer(
    struct mem_service_tcp_context *context,
    uint32_t header_timeout_ms,
    bool publish_receive,
    uint64_t *checksum_out)
{
    struct mem_service_tcp_message request;
    struct mem_service_tcp_message ack;
    struct mem_service_tcp_region_slot *destination;
    struct timespec deadline;
    uint64_t checksum;
    int status = 0;

    if (mem_service_tcp_receive_message(context,
                                        &request,
                                        header_timeout_ms) != 0 ||
        request.type != MEM_SERVICE_TCP_MESSAGE_TRANSFER ||
        request.status != 0 || request.completion_id == 0 ||
        request.region_handle == 0 || request.len == 0 ||
        request.len > SIZE_MAX || request.checksum == 0) {
        return -1;
    }
    pthread_mutex_lock(&context->region_lock);
    destination =
        mem_service_tcp_find_region(context, request.region_handle);
    if (destination == NULL) {
        pthread_mutex_unlock(&context->region_lock);
        return -1;
    }
    if (publish_receive &&
        mem_service_tcp_receive_deadline(
            context->timeout_ms, &deadline) != 0) {
        pthread_mutex_unlock(&context->region_lock);
        return -1;
    }
    while (publish_receive && destination->receive_ready &&
           mem_service_tcp_state_connected(context)) {
        if (pthread_cond_timedwait(
                &context->receive_condition,
                &context->region_lock,
                &deadline) != 0) {
            pthread_mutex_unlock(&context->region_lock);
            return -1;
        }
    }
    if (!mem_service_tcp_state_connected(context) ||
        destination->receive_ready ||
        request.offset > destination->len ||
        request.len > destination->len - request.offset ||
        mem_service_tcp_receive_all(
            context->socket_fd,
            (uint8_t *)destination->base + request.offset,
            request.len,
            context->timeout_ms) != 0) {
        pthread_mutex_unlock(&context->region_lock);
        return -1;
    }
    checksum = mem_service_provider_checksum64(
        (uint8_t *)destination->base + request.offset, request.len);
    if (checksum != request.checksum) {
        status = EILSEQ;
    }
    if (publish_receive) {
        destination->receive_ready = true;
        destination->receive_offset = request.offset;
        destination->receive_completion.id = request.completion_id;
        destination->receive_completion.status = status;
        destination->receive_completion.transferred_bytes = request.len;
        destination->receive_completion.checksum = checksum;
        pthread_cond_broadcast(&context->receive_condition);
    }
    pthread_mutex_unlock(&context->region_lock);
    memset(&ack, 0, sizeof(ack));
    ack.type = MEM_SERVICE_TCP_MESSAGE_ACK;
    ack.status = (uint32_t)status;
    ack.completion_id = request.completion_id;
    ack.region_handle = request.region_handle;
    ack.offset = request.offset;
    ack.len = request.len;
    ack.checksum = checksum;
    if (mem_service_tcp_send_message(context, &ack) != 0) {
        return -1;
    }
    if (checksum_out != NULL) {
        *checksum_out = checksum;
    }
    return status == 0 ? 0 : -1;
}

static void *mem_service_tcp_receiver_main(void *opaque)
{
    struct mem_service_tcp_context *context = opaque;

    while (mem_service_tcp_state_connected(context)) {
        if (mem_service_tcp_receive_transfer(
                context, 0, true, NULL) != 0) {
            mem_service_tcp_fail_connection(context);
            break;
        }
    }
    return NULL;
}

static int mem_service_tcp_start_receiver(
    struct mem_service_tcp_context *context)
{
    if (!context->server || context->receiver_started) {
        return -1;
    }
    if (pthread_create(&context->receiver_thread,
                       NULL,
                       mem_service_tcp_receiver_main,
                       context) != 0) {
        return -1;
    }
    context->receiver_started = true;
    return 0;
}

static int mem_service_tcp_send_transfer(
    struct mem_service_tcp_context *context,
    uint64_t completion_id,
    const void *source,
    uint64_t len,
    uint64_t destination_handle,
    uint64_t destination_offset,
    uint64_t destination_len,
    uint64_t expected_checksum)
{
    struct mem_service_tcp_message request;

    if (context == NULL || context->server || source == NULL || len == 0 ||
        destination_handle == 0 ||
        destination_offset > destination_len ||
        len > destination_len - destination_offset ||
        expected_checksum == 0 || completion_id == 0) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.type = MEM_SERVICE_TCP_MESSAGE_TRANSFER;
    request.completion_id = completion_id;
    request.region_handle = destination_handle;
    request.offset = destination_offset;
    request.len = len;
    request.checksum = expected_checksum;
    return mem_service_tcp_send_message(context, &request) != 0 ||
                   mem_service_tcp_send_all(
                       context->socket_fd, source, len) != 0
               ? -1
               : 0;
}

static int mem_service_tcp_transfer(
    struct mem_service_tcp_context *context,
    uint64_t completion_id,
    const void *source,
    uint64_t len,
    uint64_t destination_handle,
    uint64_t destination_offset,
    uint64_t destination_len,
    uint64_t expected_checksum,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_tcp_message ack;

    if (completion_out == NULL ||
        mem_service_tcp_send_transfer(
            context,
            completion_id,
            source,
            len,
            destination_handle,
            destination_offset,
            destination_len,
            expected_checksum) != 0 ||
        mem_service_tcp_receive_message(context,
                                        &ack,
                                        context->timeout_ms) != 0 ||
        ack.type != MEM_SERVICE_TCP_MESSAGE_ACK ||
        ack.status != 0 || ack.completion_id != completion_id ||
        ack.region_handle != destination_handle ||
        ack.offset != destination_offset || ack.len != len ||
        ack.checksum != expected_checksum) {
        return -1;
    }
    memset(completion_out, 0, sizeof(*completion_out));
    completion_out->id = completion_id;
    completion_out->status = 0;
    completion_out->transferred_bytes = len;
    completion_out->checksum = ack.checksum;
    return 0;
}

static int mem_service_tcp_provider_submit_transfer(
    void *opaque,
    const struct mem_service_transfer_request *request,
    uint64_t *completion_id_out)
{
    struct mem_service_tcp_context *context = opaque;
    struct mem_service_tcp_region_slot *source;
    uint64_t destination_handle;
    uint64_t destination_len;
    uint64_t completion_id;
    size_t completion_slot;
    int rc;

    if (context == NULL || request == NULL || completion_id_out == NULL ||
        !mem_service_tcp_state_connected(context) || context->server ||
        request->source.len == 0 ||
        request->source.len != request->destination.len ||
        mem_service_tcp_decode_descriptor(
            &request->destination.descriptor,
            &destination_handle,
            &destination_len) != 0) {
        return -1;
    }
    pthread_mutex_lock(&context->io_lock);
    for (completion_slot = 0;
         completion_slot < MEM_SERVICE_TCP_MAX_COMPLETIONS;
         ++completion_slot) {
        if (context->pending[completion_slot].completion.id == 0) {
            break;
        }
    }
    if (completion_slot == MEM_SERVICE_TCP_MAX_COMPLETIONS) {
        pthread_mutex_unlock(&context->io_lock);
        return -1;
    }
    pthread_mutex_lock(&context->region_lock);
    source = mem_service_tcp_find_region(
        context, request->source.region_handle);
    if (source == NULL ||
        request->source.offset > source->len ||
        request->source.len > source->len - request->source.offset) {
        pthread_mutex_unlock(&context->region_lock);
        pthread_mutex_unlock(&context->io_lock);
        return -1;
    }
    context->next_completion_id += 1U;
    completion_id = context->next_completion_id;
    rc = mem_service_tcp_send_transfer(
        context,
        completion_id,
        (uint8_t *)source->base + request->source.offset,
        request->source.len,
        destination_handle,
        request->destination.offset,
        destination_len,
        request->expected_checksum);
    pthread_mutex_unlock(&context->region_lock);
    if (rc == 0) {
        context->pending[completion_slot].completion.id = completion_id;
        context->pending[completion_slot].completion.status = EINPROGRESS;
        context->pending[completion_slot].completion.transferred_bytes =
            request->source.len;
        context->pending[completion_slot].completion.checksum =
            request->expected_checksum;
        context->pending[completion_slot].destination_handle =
            destination_handle;
        context->pending[completion_slot].destination_offset =
            request->destination.offset;
        *completion_id_out = completion_id;
    }
    pthread_mutex_unlock(&context->io_lock);
    if (rc != 0) {
        mem_service_tcp_fail_connection(context);
    }
    return rc;
}

static int mem_service_tcp_provider_poll_completion(
    void *opaque,
    uint64_t completion_id,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_tcp_context *context = opaque;
    struct mem_service_tcp_message ack;
    size_t requested_slot;
    bool failed = false;

    if (context == NULL || completion_out == NULL || completion_id == 0) {
        return -1;
    }
    pthread_mutex_lock(&context->io_lock);
    for (requested_slot = 0;
         requested_slot < MEM_SERVICE_TCP_MAX_COMPLETIONS;
         ++requested_slot) {
        if (context->pending[requested_slot].completion.id ==
            completion_id) {
            break;
        }
    }
    if (requested_slot == MEM_SERVICE_TCP_MAX_COMPLETIONS) {
        failed = true;
    }
    while (!failed &&
           context->pending[requested_slot].completion.status ==
               EINPROGRESS) {
        size_t ack_slot;

        if (mem_service_tcp_receive_message(
                context, &ack, context->timeout_ms) != 0 ||
            ack.type != MEM_SERVICE_TCP_MESSAGE_ACK ||
            ack.completion_id == 0) {
            failed = true;
            break;
        }
        for (ack_slot = 0;
             ack_slot < MEM_SERVICE_TCP_MAX_COMPLETIONS;
             ++ack_slot) {
            if (context->pending[ack_slot].completion.id ==
                ack.completion_id) {
                break;
            }
        }
        if (ack_slot == MEM_SERVICE_TCP_MAX_COMPLETIONS ||
            ack.status != 0 ||
            ack.region_handle !=
                context->pending[ack_slot].destination_handle ||
            ack.offset !=
                context->pending[ack_slot].destination_offset ||
            ack.len != context->pending[ack_slot]
                           .completion.transferred_bytes ||
            ack.checksum !=
                context->pending[ack_slot].completion.checksum) {
            failed = true;
            break;
        }
        context->pending[ack_slot].completion.status = 0;
    }
    if (!failed &&
        context->pending[requested_slot].completion.status == 0) {
        *completion_out =
            context->pending[requested_slot].completion;
        memset(&context->pending[requested_slot],
               0,
               sizeof(context->pending[requested_slot]));
    } else {
        failed = true;
    }
    pthread_mutex_unlock(&context->io_lock);
    if (failed) {
        mem_service_tcp_fail_connection(context);
        return -1;
    }
    return 0;
}

static int mem_service_tcp_take_receive_locked(
    struct mem_service_tcp_context *context,
    const struct mem_service_receive_request *request,
    uint64_t descriptor_len,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_tcp_region_slot *destination;
    bool valid;

    destination = mem_service_tcp_find_region(
        context, request->destination.region_handle);
    if (destination == NULL) {
        return -1;
    }
    if (!destination->receive_ready) {
        return 0;
    }
    valid =
        descriptor_len == destination->len &&
        request->destination.offset <= destination->len &&
        request->destination.len <=
            destination->len - request->destination.offset &&
        destination->receive_offset == request->destination.offset &&
        destination->receive_completion.status == 0 &&
        destination->receive_completion.transferred_bytes ==
            request->destination.len &&
        destination->receive_completion.checksum ==
            request->expected_checksum;
    if (valid) {
        *completion_out = destination->receive_completion;
    }
    destination->receive_ready = false;
    destination->receive_offset = 0;
    memset(&destination->receive_completion,
           0,
           sizeof(destination->receive_completion));
    pthread_cond_broadcast(&context->receive_condition);
    return valid ? 1 : -1;
}

static int mem_service_tcp_provider_wait_receive(
    void *opaque,
    const struct mem_service_receive_request *request,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_tcp_context *context = opaque;
    struct mem_service_tcp_region_slot *destination;
    struct timespec deadline;
    uint64_t descriptor_handle = 0;
    uint64_t descriptor_len = 0;
    size_t received = 0;
    int take_rc = 0;

    if (context == NULL || !context->server ||
        request == NULL || completion_out == NULL ||
        request->destination.region_handle == 0 ||
        request->destination.len == 0 ||
        request->expected_checksum == 0 ||
        mem_service_tcp_decode_descriptor(
            &request->destination.descriptor,
            &descriptor_handle,
            &descriptor_len) != 0 ||
        descriptor_handle != request->destination.region_handle ||
        mem_service_tcp_receive_deadline(
            context->timeout_ms, &deadline) != 0) {
        return -1;
    }
    if (context->receive_on_wait) {
        pthread_mutex_lock(&context->io_lock);
        while (received <= MEM_SERVICE_TCP_MAX_COMPLETIONS) {
            pthread_mutex_lock(&context->region_lock);
            take_rc = mem_service_tcp_take_receive_locked(
                context, request, descriptor_len, completion_out);
            pthread_mutex_unlock(&context->region_lock);
            if (take_rc != 0) {
                break;
            }
            if (received == MEM_SERVICE_TCP_MAX_COMPLETIONS ||
                mem_service_tcp_receive_transfer(
                    context,
                    context->timeout_ms,
                    true,
                    NULL) != 0) {
                take_rc = -1;
                break;
            }
            received += 1U;
        }
        pthread_mutex_unlock(&context->io_lock);
    } else {
        pthread_mutex_lock(&context->region_lock);
        destination = mem_service_tcp_find_region(
            context, request->destination.region_handle);
        while (destination != NULL && !destination->receive_ready &&
               mem_service_tcp_state_connected(context)) {
            if (pthread_cond_timedwait(
                    &context->receive_condition,
                    &context->region_lock,
                    &deadline) != 0) {
                break;
            }
        }
        take_rc = mem_service_tcp_take_receive_locked(
            context, request, descriptor_len, completion_out);
        pthread_mutex_unlock(&context->region_lock);
    }
    if (take_rc != 1) {
        mem_service_tcp_fail_connection(context);
        return -1;
    }
    return 0;
}

static const struct mem_service_provider_ops mem_service_tcp_provider_ops = {
    .probe = mem_service_tcp_provider_probe,
    .register_region = mem_service_tcp_provider_register_region,
    .deregister_region = mem_service_tcp_provider_deregister_region,
    .submit_transfer = mem_service_tcp_provider_submit_transfer,
    .poll_completion = mem_service_tcp_provider_poll_completion,
    .wait_receive = mem_service_tcp_provider_wait_receive,
};

static struct mem_service_tcp_context *mem_service_tcp_context_create(
    const struct mem_service_provider_tcp_config *config,
    bool server)
{
    struct mem_service_tcp_context *context;

    if (config == NULL || config->local_ipv4 == NULL ||
        config->peer_ipv4 == NULL || config->port == 0 ||
        config->timeout_ms == 0) {
        return NULL;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return NULL;
    }
    context->listen_fd = -1;
    context->socket_fd = -1;
    context->timeout_ms = config->timeout_ms;
    context->server = server;
    context->receive_on_wait = config->receive_on_wait;
    snprintf(context->local_ipv4, sizeof(context->local_ipv4),
             "%s", config->local_ipv4);
    snprintf(context->peer_ipv4, sizeof(context->peer_ipv4),
             "%s", config->peer_ipv4);
    snprintf(context->instance, sizeof(context->instance),
             "%s:%u", config->peer_ipv4, (unsigned int)config->port);
    if (pthread_mutex_init(&context->state_lock, NULL) != 0) {
        free(context);
        return NULL;
    }
    if (pthread_mutex_init(&context->region_lock, NULL) != 0) {
        pthread_mutex_destroy(&context->state_lock);
        free(context);
        return NULL;
    }
    if (pthread_mutex_init(&context->io_lock, NULL) != 0) {
        pthread_mutex_destroy(&context->region_lock);
        pthread_mutex_destroy(&context->state_lock);
        free(context);
        return NULL;
    }
    if (pthread_cond_init(&context->receive_condition, NULL) != 0) {
        pthread_mutex_destroy(&context->io_lock);
        pthread_mutex_destroy(&context->region_lock);
        pthread_mutex_destroy(&context->state_lock);
        free(context);
        return NULL;
    }
    return context;
}

static void mem_service_tcp_context_destroy(
    struct mem_service_tcp_context *context)
{
    if (context == NULL) {
        return;
    }
    mem_service_tcp_fail_connection(context);
    if (context->receiver_started) {
        (void)pthread_join(context->receiver_thread, NULL);
    }
    if (context->socket_fd >= 0) {
        close(context->socket_fd);
    }
    if (context->listen_fd >= 0) {
        close(context->listen_fd);
    }
    pthread_cond_destroy(&context->receive_condition);
    pthread_mutex_destroy(&context->io_lock);
    pthread_mutex_destroy(&context->region_lock);
    pthread_mutex_destroy(&context->state_lock);
    free(context);
}

static int mem_service_tcp_listen(
    struct mem_service_tcp_context *context,
    const struct mem_service_provider_tcp_config *config)
{
    struct sockaddr_in address;
    int reuse = 1;

    if (mem_service_tcp_fill_address(config->local_ipv4,
                                     config->port,
                                     &address) != 0) {
        return -1;
    }
    context->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (context->listen_fd < 0 ||
        setsockopt(context->listen_fd,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &reuse,
                   sizeof(reuse)) != 0 ||
        bind(context->listen_fd,
             (struct sockaddr *)&address,
             sizeof(address)) != 0 ||
        listen(context->listen_fd, 1) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_tcp_accept(
    struct mem_service_tcp_context *context)
{
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    char peer_ipv4[INET_ADDRSTRLEN];

    if (context->listen_fd < 0 ||
        mem_service_tcp_wait_fd(context->listen_fd,
                                POLLIN,
                                context->timeout_ms) != 0) {
        return -1;
    }
    context->socket_fd = accept(context->listen_fd,
                                (struct sockaddr *)&peer,
                                &peer_len);
    if (context->socket_fd < 0) {
        return -1;
    }
    if (inet_ntop(AF_INET,
                  &peer.sin_addr,
                  peer_ipv4,
                  sizeof(peer_ipv4)) == NULL) {
        return -1;
    }
    if (strcmp(peer_ipv4, context->peer_ipv4) != 0) {
        fprintf(stderr,
                "mem_service tcp-provider: stage=accept-peer "
                "actual=%s expected=%s\n",
                peer_ipv4,
                context->peer_ipv4);
        return -1;
    }
    if (mem_service_tcp_configure_socket(
            context->socket_fd, context->timeout_ms) != 0) {
        (void)mem_service_tcp_error("accept-socket-options");
        return -1;
    }
    close(context->listen_fd);
    context->listen_fd = -1;
    mem_service_tcp_set_state(context, true, false);
    return 0;
}

static int mem_service_tcp_connect(
    struct mem_service_tcp_context *context,
    const struct mem_service_provider_tcp_config *config)
{
    struct sockaddr_in local;
    struct sockaddr_in peer;
    int flags;
    int socket_error = 0;
    socklen_t error_len = sizeof(socket_error);

    if (mem_service_tcp_fill_address(config->local_ipv4, 0, &local) != 0 ||
        mem_service_tcp_fill_address(config->peer_ipv4,
                                     config->port,
                                     &peer) != 0) {
        return -1;
    }
    context->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (context->socket_fd < 0 ||
        bind(context->socket_fd,
             (struct sockaddr *)&local,
             sizeof(local)) != 0 ||
        (flags = fcntl(context->socket_fd, F_GETFL, 0)) < 0 ||
        fcntl(context->socket_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        return -1;
    }
    if (connect(context->socket_fd,
                (struct sockaddr *)&peer,
                sizeof(peer)) != 0 &&
        errno != EINPROGRESS) {
        return -1;
    }
    if (mem_service_tcp_wait_fd(context->socket_fd,
                                POLLOUT,
                                context->timeout_ms) != 0) {
        return mem_service_tcp_error("connect-wait");
    }
    if (getsockopt(context->socket_fd,
                   SOL_SOCKET,
                   SO_ERROR,
                   &socket_error,
                   &error_len) != 0 ||
        socket_error != 0) {
        errno = socket_error;
        return mem_service_tcp_error("connect-result");
    }
    if (fcntl(context->socket_fd, F_SETFL, flags) != 0) {
        return mem_service_tcp_error("connect-blocking");
    }
    if (mem_service_tcp_configure_socket(
            context->socket_fd, context->timeout_ms) != 0) {
        (void)mem_service_tcp_error("connect-socket-options");
        return -1;
    }
    mem_service_tcp_set_state(context, true, false);
    return 0;
}

int mem_service_provider_tcp_endpoint_listen(
    struct mem_service_provider_tcp_endpoint *endpoint,
    const struct mem_service_provider_tcp_config *config)
{
    struct mem_service_tcp_context *context;

    if (endpoint == NULL || endpoint->implementation != NULL) {
        return -1;
    }
    context = mem_service_tcp_context_create(config, true);
    if (context == NULL || mem_service_tcp_listen(context, config) != 0) {
        (void)mem_service_tcp_error("listen");
        mem_service_tcp_context_destroy(context);
        return -1;
    }
    endpoint->implementation = context;
    return 0;
}

int mem_service_provider_tcp_endpoint_accept(
    struct mem_service_provider_tcp_endpoint *endpoint,
    const struct mem_service_provider_tcp_config *config)
{
    struct mem_service_tcp_context *context;

    (void)config;
    if (endpoint == NULL || endpoint->implementation == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    if (mem_service_tcp_accept(context) != 0) {
        return mem_service_tcp_error("accept");
    }
    return 0;
}

int mem_service_provider_tcp_endpoint_open(
    struct mem_service_provider_tcp_endpoint *endpoint,
    const struct mem_service_provider_tcp_config *config,
    bool server)
{
    struct mem_service_tcp_context *context;

    if (endpoint == NULL || endpoint->implementation != NULL) {
        return -1;
    }
    if (server) {
        if (mem_service_provider_tcp_endpoint_listen(endpoint, config) != 0 ||
            mem_service_provider_tcp_endpoint_accept(endpoint, config) != 0) {
            mem_service_provider_tcp_endpoint_close(endpoint);
            return -1;
        }
        return 0;
    }
    context = mem_service_tcp_context_create(config, false);
    if (context == NULL || mem_service_tcp_connect(context, config) != 0) {
        (void)mem_service_tcp_error("connect");
        mem_service_tcp_context_destroy(context);
        return -1;
    }
    endpoint->implementation = context;
    return 0;
}

int mem_service_provider_tcp_endpoint_registration(
    struct mem_service_provider_tcp_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out)
{
    struct mem_service_tcp_context *context;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        registration_out == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    memset(registration_out, 0, sizeof(*registration_out));
    registration_out->name = "tcp";
    registration_out->instance = context->instance;
    registration_out->capabilities =
        MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
        MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER |
        MEM_SERVICE_PROVIDER_CAP_RECEIVE_FENCE;
    registration_out->ops = &mem_service_tcp_provider_ops;
    registration_out->context = context;
    return 0;
}

void mem_service_provider_tcp_endpoint_close(
    struct mem_service_provider_tcp_endpoint *endpoint)
{
    if (endpoint == NULL || endpoint->implementation == NULL) {
        return;
    }
    mem_service_tcp_context_destroy(endpoint->implementation);
    endpoint->implementation = NULL;
}

static void mem_service_tcp_fill_payload(uint8_t *payload,
                                         uint64_t len,
                                         uint64_t iteration)
{
    uint64_t i;

    for (i = 0; i < len; ++i) {
        payload[i] =
            (uint8_t)((i * 131U + iteration * 17U + 0x5aU) & 0xffU);
    }
}

static void mem_service_tcp_fill_result(
    const struct mem_service_tcp_context *context,
    uint64_t payload_bytes,
    uint32_t iterations,
    uint64_t checksum,
    uint64_t elapsed_us,
    struct mem_service_provider_tcp_canary_result *result)
{
    memset(result, 0, sizeof(*result));
    snprintf(result->local_ipv4, sizeof(result->local_ipv4),
             "%s", context->local_ipv4);
    snprintf(result->peer_ipv4, sizeof(result->peer_ipv4),
             "%s", context->peer_ipv4);
    result->payload_bytes = payload_bytes;
    result->iterations = iterations;
    result->checksum = checksum;
    result->elapsed_us = elapsed_us;
    result->data_plane_ready = true;
}

static int mem_service_tcp_verify_server(
    struct mem_service_tcp_context *context,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_tcp_canary_result *result)
{
    uint8_t *payload = NULL;
    struct mem_service_region_request request;
    struct mem_service_region region;
    struct mem_service_tcp_message region_message;
    uint64_t start_us;
    uint64_t checksum = 0;
    uint32_t i;
    int rc = -1;

    payload = calloc(1, (size_t)payload_bytes);
    if (payload == NULL) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.base = payload;
    request.len = payload_bytes;
    request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    if (mem_service_tcp_provider_register_region(
            context, &request, &region) != 0) {
        (void)mem_service_tcp_error("verify-server-register");
        goto done;
    }
    memset(&region_message, 0, sizeof(region_message));
    region_message.type = MEM_SERVICE_TCP_MESSAGE_REGION;
    region_message.region_handle = region.handle;
    region_message.len = region.len;
    if (mem_service_tcp_send_message(context, &region_message) != 0) {
        (void)mem_service_tcp_error("verify-server-send-region");
        goto deregister;
    }
    start_us = mem_service_tcp_now_us();
    for (i = 0; i < iterations; ++i) {
        if (mem_service_tcp_receive_transfer(
                context,
                context->timeout_ms,
                false,
                &checksum) != 0) {
            (void)mem_service_tcp_error("verify-server-receive-transfer");
            goto deregister;
        }
    }
    mem_service_tcp_fill_result(context,
                                payload_bytes,
                                iterations,
                                checksum,
                                mem_service_tcp_now_us() - start_us,
                                result);
    rc = 0;

deregister:
    if (mem_service_tcp_provider_deregister_region(
            context, region.handle) != 0) {
        rc = -1;
    }
done:
    free(payload);
    return rc;
}

static int mem_service_tcp_verify_client(
    struct mem_service_tcp_context *context,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_tcp_canary_result *result)
{
    uint8_t *payload = NULL;
    struct mem_service_tcp_message region_message;
    struct mem_service_region_request request;
    struct mem_service_region region;
    struct mem_service_transfer_completion completion;
    uint64_t start_us;
    uint64_t checksum = 0;
    uint32_t i;
    int rc = -1;

    if (mem_service_tcp_receive_message(context,
                                        &region_message,
                                        context->timeout_ms) != 0 ||
        region_message.type != MEM_SERVICE_TCP_MESSAGE_REGION ||
        region_message.status != 0 ||
        region_message.region_handle == 0 ||
        region_message.len < payload_bytes) {
        return mem_service_tcp_error("verify-client-receive-region");
    }
    payload = malloc((size_t)payload_bytes);
    if (payload == NULL) {
        return -1;
    }
    memset(&request, 0, sizeof(request));
    request.base = payload;
    request.len = payload_bytes;
    request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    if (mem_service_tcp_provider_register_region(
            context, &request, &region) != 0) {
        (void)mem_service_tcp_error("verify-client-register");
        goto done;
    }
    start_us = mem_service_tcp_now_us();
    for (i = 0; i < iterations; ++i) {
        mem_service_tcp_fill_payload(payload, payload_bytes, i);
        checksum = mem_service_provider_checksum64(payload, payload_bytes);
        context->next_completion_id += 1U;
        if (mem_service_tcp_transfer(
                context,
                context->next_completion_id,
                payload,
                payload_bytes,
                region_message.region_handle,
                0,
                region_message.len,
                checksum,
                &completion) != 0) {
            (void)mem_service_tcp_error("verify-client-transfer");
            goto deregister;
        }
    }
    mem_service_tcp_fill_result(context,
                                payload_bytes,
                                iterations,
                                checksum,
                                mem_service_tcp_now_us() - start_us,
                                result);
    rc = 0;

deregister:
    if (mem_service_tcp_provider_deregister_region(
            context, region.handle) != 0) {
        rc = -1;
    }
done:
    free(payload);
    return rc;
}

int mem_service_provider_tcp_endpoint_verify(
    struct mem_service_provider_tcp_endpoint *endpoint,
    bool server,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_tcp_canary_result *result)
{
    struct mem_service_tcp_context *context;
    int rc;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        payload_bytes == 0 || payload_bytes > SIZE_MAX ||
        iterations == 0 || result == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    if (context->server != server ||
        !mem_service_tcp_state_connected(context)) {
        return -1;
    }
    rc = server
             ? mem_service_tcp_verify_server(
                   context, payload_bytes, iterations, result)
             : mem_service_tcp_verify_client(
                   context, payload_bytes, iterations, result);
    if (rc != 0) {
        mem_service_tcp_fail_connection(context);
        return -1;
    }
    mem_service_tcp_set_state(context, true, true);
    if (server && !context->receive_on_wait &&
        mem_service_tcp_start_receiver(context) != 0) {
        mem_service_tcp_fail_connection(context);
        return -1;
    }
    return 0;
}

int mem_service_provider_tcp_run_server_canary(
    const struct mem_service_provider_tcp_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_tcp_canary_result *result)
{
    struct mem_service_provider_tcp_endpoint endpoint = {0};
    int rc;

    if (mem_service_provider_tcp_endpoint_open(
            &endpoint, config, true) != 0) {
        return -1;
    }
    rc = mem_service_provider_tcp_endpoint_verify(
        &endpoint, true, payload_bytes, iterations, result);
    mem_service_provider_tcp_endpoint_close(&endpoint);
    return rc;
}

int mem_service_provider_tcp_run_client_canary(
    const struct mem_service_provider_tcp_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_tcp_canary_result *result)
{
    struct mem_service_provider_tcp_endpoint endpoint = {0};
    int rc;

    if (mem_service_provider_tcp_endpoint_open(
            &endpoint, config, false) != 0) {
        return -1;
    }
    rc = mem_service_provider_tcp_endpoint_verify(
        &endpoint, false, payload_bytes, iterations, result);
    mem_service_provider_tcp_endpoint_close(&endpoint);
    return rc;
}

int mem_service_provider_tcp_run_protocol_fixture(void)
{
    struct mem_service_provider_descriptor opaque;
    struct mem_service_tcp_descriptor_wire descriptor;
    uint64_t handle;
    uint64_t len;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = htonl(MEM_SERVICE_TCP_DESCRIPTOR_MAGIC);
    descriptor.version = htonl(MEM_SERVICE_TCP_PROTOCOL_VERSION);
    descriptor.region_handle = mem_service_tcp_hton64(0x1020304050607080ULL);
    descriptor.len = mem_service_tcp_hton64(4096);
    memset(&opaque, 0, sizeof(opaque));
    opaque.len = sizeof(descriptor);
    memcpy(opaque.bytes, &descriptor, sizeof(descriptor));
    if (mem_service_tcp_decode_descriptor(&opaque, &handle, &len) != 0 ||
        handle != 0x1020304050607080ULL || len != 4096) {
        return 1;
    }
    opaque.bytes[0] ^= 0xffU;
    if (mem_service_tcp_decode_descriptor(&opaque, &handle, &len) == 0) {
        return 1;
    }
    printf("mem_service tcp-provider-fixtures: status=ok "
           "protocol_version=%u descriptor_bytes=%zu "
           "corruption=fail-closed\n",
           MEM_SERVICE_TCP_PROTOCOL_VERSION,
           sizeof(descriptor));
    return 0;
}
