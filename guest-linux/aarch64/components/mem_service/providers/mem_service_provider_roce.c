#include "mem_service_provider_roce.h"

#include <arpa/inet.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <rdma/rdma_cma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MEM_SERVICE_ROCE_DESCRIPTOR_MAGIC 0x4d535244U
#define MEM_SERVICE_ROCE_CONTROL_MAGIC 0x4d535243U
#define MEM_SERVICE_ROCE_PROTOCOL_VERSION 1U
#define MEM_SERVICE_ROCE_MAX_REGIONS 16U
#define MEM_SERVICE_ROCE_MAX_PENDING_WC 16U
#define MEM_SERVICE_ROCE_CONTROL_RECV_WR_ID 1U
#define MEM_SERVICE_ROCE_CONTROL_SEND_WR_ID 2U
#define MEM_SERVICE_ROCE_TRANSFER_WR_ID_BASE (1ULL << 32)

enum mem_service_roce_control_type {
    MEM_SERVICE_ROCE_CONTROL_REGION = 1,
    MEM_SERVICE_ROCE_CONTROL_DONE = 2,
    MEM_SERVICE_ROCE_CONTROL_ACK = 3,
};

struct mem_service_roce_descriptor_wire {
    uint32_t magic;
    uint32_t version;
    uint32_t rkey;
    uint32_t reserved;
    uint64_t address;
    uint64_t len;
};

struct mem_service_roce_control_wire {
    uint32_t magic;
    uint32_t version;
    uint32_t type;
    uint32_t status;
    uint64_t address;
    uint64_t len;
    uint64_t checksum;
    uint64_t iteration;
    uint32_t rkey;
    uint32_t reserved;
};

struct mem_service_roce_region_slot {
    bool in_use;
    uint64_t handle;
    void *base;
    uint64_t len;
    struct ibv_mr *mr;
};

struct mem_service_roce_context {
    struct rdma_event_channel *event_channel;
    struct rdma_cm_id *listen_id;
    struct rdma_cm_id *id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_mr *control_send_mr;
    struct ibv_mr *control_recv_mr;
    struct mem_service_roce_control_wire control_send;
    struct mem_service_roce_control_wire control_recv;
    struct mem_service_roce_region_slot regions[MEM_SERVICE_ROCE_MAX_REGIONS];
    struct ibv_wc pending_wc[MEM_SERVICE_ROCE_MAX_PENDING_WC];
    size_t pending_wc_count;
    uint64_t next_region_handle;
    uint64_t next_completion_id;
    uint64_t pending_completion_id;
    uint64_t pending_transfer_bytes;
    uint64_t pending_transfer_checksum;
    uint32_t timeout_ms;
    char device[64];
    char local_ipv4[48];
    char peer_ipv4[48];
    char instance[64];
    bool connected;
    bool transfer_verified;
};

static uint64_t mem_service_roce_hton64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)value) << 32);
#else
    return value;
#endif
}

static uint64_t mem_service_roce_ntoh64(uint64_t value)
{
    return mem_service_roce_hton64(value);
}

static uint64_t mem_service_roce_checksum(const void *data, uint64_t len)
{
    const uint8_t *bytes = data;
    uint64_t checksum = 1469598103934665603ULL;
    uint64_t i;

    for (i = 0; i < len; ++i) {
        checksum ^= bytes[i];
        checksum *= 1099511628211ULL;
    }
    return checksum;
}

static uint64_t mem_service_roce_now_us(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL +
           (uint64_t)now.tv_nsec / 1000ULL;
}

static unsigned int mem_service_roce_mtu_bytes(enum ibv_mtu mtu)
{
    switch (mtu) {
    case IBV_MTU_256:
        return 256U;
    case IBV_MTU_512:
        return 512U;
    case IBV_MTU_1024:
        return 1024U;
    case IBV_MTU_2048:
        return 2048U;
    case IBV_MTU_4096:
        return 4096U;
    default:
        return 0U;
    }
}

static int mem_service_roce_fill_address(const char *ipv4,
                                         uint16_t port,
                                         struct sockaddr_in *address)
{
    if (ipv4 == NULL || ipv4[0] == '\0' || address == NULL || port == 0) {
        return -1;
    }
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons(port);
    return inet_pton(AF_INET, ipv4, &address->sin_addr) == 1 ? 0 : -1;
}

static int mem_service_roce_wait_event(struct rdma_event_channel *channel,
                                       enum rdma_cm_event_type expected,
                                       struct rdma_cm_id **id_out,
                                       uint32_t timeout_ms)
{
    struct rdma_cm_event *event = NULL;
    enum rdma_cm_event_type actual;
    struct pollfd event_poll;
    struct rdma_cm_id *id;
    int poll_rc;
    int status;

    if (channel == NULL || timeout_ms == 0) {
        return -1;
    }
    memset(&event_poll, 0, sizeof(event_poll));
    event_poll.fd = channel->fd;
    event_poll.events = POLLIN;
    do {
        poll_rc = poll(&event_poll, 1, (int)timeout_ms);
    } while (poll_rc < 0 && errno == EINTR);
    if (poll_rc <= 0 || (event_poll.revents & POLLIN) == 0 ||
        rdma_get_cm_event(channel, &event) != 0) {
        return -1;
    }
    actual = event->event;
    status = event->status;
    id = event->id;
    if (rdma_ack_cm_event(event) != 0) {
        return -1;
    }
    if (actual != expected || status != 0) {
        return -1;
    }
    if (id_out != NULL) {
        *id_out = id;
    }
    return 0;
}

static int mem_service_roce_pending_take(
    struct mem_service_roce_context *context,
    uint64_t wr_id,
    struct ibv_wc *completion)
{
    size_t i;

    for (i = 0; i < context->pending_wc_count; ++i) {
        if (context->pending_wc[i].wr_id == wr_id) {
            *completion = context->pending_wc[i];
            context->pending_wc[i] =
                context->pending_wc[context->pending_wc_count - 1U];
            context->pending_wc_count -= 1U;
            return 1;
        }
    }
    return 0;
}

static int mem_service_roce_wait_completion(
    struct mem_service_roce_context *context,
    uint64_t wr_id,
    struct ibv_wc *completion)
{
    uint64_t start_us;
    uint64_t timeout_us;

    if (context == NULL || context->cq == NULL || completion == NULL) {
        return -1;
    }
    if (mem_service_roce_pending_take(context, wr_id, completion) == 1) {
        return completion->status == IBV_WC_SUCCESS ? 0 : -1;
    }
    start_us = mem_service_roce_now_us();
    timeout_us = (uint64_t)context->timeout_ms * 1000ULL;
    for (;;) {
        struct ibv_wc polled[4];
        struct ibv_wc matched;
        bool found = false;
        int count = ibv_poll_cq(context->cq, 4, polled);
        int i;

        if (count < 0) {
            return -1;
        }
        for (i = 0; i < count; ++i) {
            if (!found && polled[i].wr_id == wr_id) {
                matched = polled[i];
                found = true;
                continue;
            }
            if (context->pending_wc_count >= MEM_SERVICE_ROCE_MAX_PENDING_WC) {
                return -1;
            }
            context->pending_wc[context->pending_wc_count++] = polled[i];
        }
        if (found) {
            *completion = matched;
            return completion->status == IBV_WC_SUCCESS ? 0 : -1;
        }
        if (mem_service_roce_now_us() - start_us >= timeout_us) {
            return -1;
        }
        usleep(100);
    }
}

static int mem_service_roce_post_control_receive(
    struct mem_service_roce_context *context)
{
    struct ibv_sge scatter;
    struct ibv_recv_wr receive;
    struct ibv_recv_wr *bad = NULL;

    memset(&scatter, 0, sizeof(scatter));
    scatter.addr = (uintptr_t)&context->control_recv;
    scatter.length = sizeof(context->control_recv);
    scatter.lkey = context->control_recv_mr->lkey;
    memset(&receive, 0, sizeof(receive));
    receive.wr_id = MEM_SERVICE_ROCE_CONTROL_RECV_WR_ID;
    receive.sg_list = &scatter;
    receive.num_sge = 1;
    return ibv_post_recv(context->id->qp, &receive, &bad);
}

static int mem_service_roce_create_resources(
    struct mem_service_roce_context *context)
{
    struct ibv_qp_init_attr qp_attributes;

    context->pd = ibv_alloc_pd(context->id->verbs);
    if (context->pd == NULL) {
        return -1;
    }
    context->cq = ibv_create_cq(context->id->verbs, 64, NULL, NULL, 0);
    if (context->cq == NULL) {
        return -1;
    }
    memset(&qp_attributes, 0, sizeof(qp_attributes));
    qp_attributes.qp_type = IBV_QPT_RC;
    qp_attributes.send_cq = context->cq;
    qp_attributes.recv_cq = context->cq;
    qp_attributes.cap.max_send_wr = 32;
    qp_attributes.cap.max_recv_wr = 32;
    qp_attributes.cap.max_send_sge = 1;
    qp_attributes.cap.max_recv_sge = 1;
    if (rdma_create_qp(context->id, context->pd, &qp_attributes) != 0) {
        return -1;
    }
    context->control_send_mr =
        ibv_reg_mr(context->pd,
                   &context->control_send,
                   sizeof(context->control_send),
                   IBV_ACCESS_LOCAL_WRITE);
    context->control_recv_mr =
        ibv_reg_mr(context->pd,
                   &context->control_recv,
                   sizeof(context->control_recv),
                   IBV_ACCESS_LOCAL_WRITE);
    if (context->control_send_mr == NULL ||
        context->control_recv_mr == NULL ||
        mem_service_roce_post_control_receive(context) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_roce_capture_identity(
    struct mem_service_roce_context *context,
    const char *expected_device)
{
    const struct sockaddr *local_address;
    const struct sockaddr *peer_address;
    const char *device;

    if (context == NULL || context->id == NULL || context->id->verbs == NULL) {
        return -1;
    }
    device = ibv_get_device_name(context->id->verbs->device);
    if (device == NULL ||
        (expected_device != NULL && expected_device[0] != '\0' &&
         strcmp(expected_device, device) != 0)) {
        return -1;
    }
    snprintf(context->device, sizeof(context->device), "%s", device);
    local_address = rdma_get_local_addr(context->id);
    peer_address = rdma_get_peer_addr(context->id);
    if (local_address == NULL || peer_address == NULL ||
        local_address->sa_family != AF_INET ||
        peer_address->sa_family != AF_INET ||
        inet_ntop(AF_INET,
                  &((const struct sockaddr_in *)local_address)->sin_addr,
                  context->local_ipv4,
                  sizeof(context->local_ipv4)) == NULL ||
        inet_ntop(AF_INET,
                  &((const struct sockaddr_in *)peer_address)->sin_addr,
                  context->peer_ipv4,
                  sizeof(context->peer_ipv4)) == NULL) {
        return -1;
    }
    if (snprintf(context->instance,
                 sizeof(context->instance),
                 "%s:%s",
                 device,
                 context->peer_ipv4) >= (int)sizeof(context->instance)) {
        return -1;
    }
    return 0;
}

static void mem_service_roce_context_destroy(
    struct mem_service_roce_context *context)
{
    size_t i;

    if (context == NULL) {
        return;
    }
    if (context->id != NULL && context->id->qp != NULL) {
        rdma_destroy_qp(context->id);
    }
    for (i = 0; i < MEM_SERVICE_ROCE_MAX_REGIONS; ++i) {
        if (context->regions[i].mr != NULL) {
            ibv_dereg_mr(context->regions[i].mr);
        }
    }
    if (context->control_recv_mr != NULL) {
        ibv_dereg_mr(context->control_recv_mr);
    }
    if (context->control_send_mr != NULL) {
        ibv_dereg_mr(context->control_send_mr);
    }
    if (context->cq != NULL) {
        ibv_destroy_cq(context->cq);
    }
    if (context->pd != NULL) {
        ibv_dealloc_pd(context->pd);
    }
    if (context->id != NULL) {
        rdma_destroy_id(context->id);
    }
    if (context->listen_id != NULL) {
        rdma_destroy_id(context->listen_id);
    }
    if (context->event_channel != NULL) {
        rdma_destroy_event_channel(context->event_channel);
    }
    memset(context, 0, sizeof(*context));
}

static int mem_service_roce_listen_server(
    struct mem_service_roce_context *context,
    const struct mem_service_provider_roce_config *config)
{
    struct sockaddr_in local_address;

    if (context == NULL || config == NULL ||
        mem_service_roce_fill_address(config->local_ipv4,
                                      config->port,
                                      &local_address) != 0) {
        return -1;
    }
    memset(context, 0, sizeof(*context));
    context->timeout_ms = config->timeout_ms == 0 ? 10000U : config->timeout_ms;
    context->event_channel = rdma_create_event_channel();
    if (context->event_channel == NULL ||
        rdma_create_id(context->event_channel,
                       &context->listen_id,
                       NULL,
                       RDMA_PS_TCP) != 0 ||
        rdma_bind_addr(context->listen_id,
                       (struct sockaddr *)&local_address) != 0 ||
        rdma_listen(context->listen_id, 8) != 0) {
        return -1;
    }
    return 0;
}

static int mem_service_roce_accept_server(
    struct mem_service_roce_context *context,
    const struct mem_service_provider_roce_config *config)
{
    struct rdma_cm_id *accepted = NULL;
    struct rdma_conn_param connection;

    if (context == NULL || config == NULL ||
        context->event_channel == NULL || context->listen_id == NULL ||
        context->id != NULL ||
        mem_service_roce_wait_event(context->event_channel,
                                    RDMA_CM_EVENT_CONNECT_REQUEST,
                                    &accepted,
                                    context->timeout_ms) != 0) {
        return -1;
    }
    context->id = accepted;
    if (mem_service_roce_capture_identity(context, config->expected_device) != 0 ||
        (config->peer_ipv4 != NULL && config->peer_ipv4[0] != '\0' &&
         strcmp(config->peer_ipv4, context->peer_ipv4) != 0) ||
        mem_service_roce_create_resources(context) != 0) {
        return -1;
    }
    memset(&connection, 0, sizeof(connection));
    connection.initiator_depth = 4;
    connection.responder_resources = 4;
    connection.rnr_retry_count = 7;
    if (rdma_accept(context->id, &connection) != 0 ||
        mem_service_roce_wait_event(context->event_channel,
                                    RDMA_CM_EVENT_ESTABLISHED,
                                    NULL,
                                    context->timeout_ms) != 0) {
        return -1;
    }
    context->connected = true;
    return 0;
}

static int mem_service_roce_open_client(
    struct mem_service_roce_context *context,
    const struct mem_service_provider_roce_config *config)
{
    struct sockaddr_in local_address;
    struct sockaddr_in peer_address;
    struct rdma_conn_param connection;

    if (context == NULL || config == NULL ||
        mem_service_roce_fill_address(config->local_ipv4,
                                      config->port,
                                      &local_address) != 0 ||
        mem_service_roce_fill_address(config->peer_ipv4,
                                      config->port,
                                      &peer_address) != 0) {
        return -1;
    }
    local_address.sin_port = 0;
    memset(context, 0, sizeof(*context));
    context->timeout_ms = config->timeout_ms == 0 ? 10000U : config->timeout_ms;
    context->event_channel = rdma_create_event_channel();
    if (context->event_channel == NULL ||
        rdma_create_id(context->event_channel,
                       &context->id,
                       NULL,
                       RDMA_PS_TCP) != 0 ||
        rdma_resolve_addr(context->id,
                          (struct sockaddr *)&local_address,
                          (struct sockaddr *)&peer_address,
                          (int)context->timeout_ms) != 0 ||
        mem_service_roce_wait_event(context->event_channel,
                                    RDMA_CM_EVENT_ADDR_RESOLVED,
                                    NULL,
                                    context->timeout_ms) != 0 ||
        mem_service_roce_capture_identity(context, config->expected_device) != 0 ||
        rdma_resolve_route(context->id, (int)context->timeout_ms) != 0 ||
        mem_service_roce_wait_event(context->event_channel,
                                    RDMA_CM_EVENT_ROUTE_RESOLVED,
                                    NULL,
                                    context->timeout_ms) != 0 ||
        mem_service_roce_create_resources(context) != 0) {
        return -1;
    }
    memset(&connection, 0, sizeof(connection));
    connection.initiator_depth = 4;
    connection.responder_resources = 4;
    connection.retry_count = 7;
    connection.rnr_retry_count = 7;
    if (rdma_connect(context->id, &connection) != 0 ||
        mem_service_roce_wait_event(context->event_channel,
                                    RDMA_CM_EVENT_ESTABLISHED,
                                    NULL,
                                    context->timeout_ms) != 0) {
        return -1;
    }
    context->connected = true;
    return 0;
}

static struct mem_service_roce_region_slot *mem_service_roce_find_region(
    struct mem_service_roce_context *context,
    uint64_t handle)
{
    size_t i;

    for (i = 0; i < MEM_SERVICE_ROCE_MAX_REGIONS; ++i) {
        if (context->regions[i].in_use &&
            context->regions[i].handle == handle) {
            return &context->regions[i];
        }
    }
    return NULL;
}

static bool mem_service_roce_connection_live(
    struct mem_service_roce_context *context)
{
    struct ibv_qp_attr attributes;
    struct ibv_qp_init_attr initial;
    struct pollfd event_poll;

    if (context == NULL || !context->connected || context->id == NULL ||
        context->id->qp == NULL || context->event_channel == NULL) {
        return false;
    }
    memset(&event_poll, 0, sizeof(event_poll));
    event_poll.fd = context->event_channel->fd;
    event_poll.events = POLLIN;
    if (poll(&event_poll, 1, 0) > 0 &&
        (event_poll.revents & POLLIN) != 0) {
        struct rdma_cm_event *event = NULL;

        if (rdma_get_cm_event(context->event_channel, &event) != 0) {
            return false;
        }
        if (event->event == RDMA_CM_EVENT_DISCONNECTED ||
            event->event == RDMA_CM_EVENT_DEVICE_REMOVAL ||
            event->event == RDMA_CM_EVENT_ADDR_CHANGE ||
            event->status != 0) {
            context->connected = false;
        }
        if (rdma_ack_cm_event(event) != 0) {
            context->connected = false;
        }
    }
    memset(&attributes, 0, sizeof(attributes));
    memset(&initial, 0, sizeof(initial));
    if (ibv_query_qp(context->id->qp,
                     &attributes,
                     IBV_QP_STATE,
                     &initial) != 0 ||
        attributes.qp_state != IBV_QPS_RTS) {
        context->connected = false;
    }
    return context->connected;
}

static int mem_service_roce_provider_probe(
    void *opaque,
    enum mem_service_provider_state *state_out)
{
    struct mem_service_roce_context *context = opaque;

    if (context == NULL || state_out == NULL) {
        return -1;
    }
    if (!mem_service_roce_connection_live(context)) {
        *state_out = MEM_SERVICE_PROVIDER_STATE_UNAVAILABLE;
    } else if (!context->transfer_verified) {
        *state_out = MEM_SERVICE_PROVIDER_STATE_DEGRADED;
    } else {
        *state_out = MEM_SERVICE_PROVIDER_STATE_READY;
    }
    return 0;
}

static int mem_service_roce_provider_register_region(
    void *opaque,
    const struct mem_service_region_request *request,
    struct mem_service_region *region_out)
{
    struct mem_service_roce_context *context = opaque;
    struct mem_service_roce_descriptor_wire descriptor;
    struct mem_service_roce_region_slot *slot = NULL;
    size_t i;

    if (context == NULL || !context->connected || request == NULL ||
        region_out == NULL || request->base == NULL || request->len == 0 ||
        request->len > SIZE_MAX ||
        request->memory_kind != MEM_SERVICE_MEMORY_HOST) {
        return -1;
    }
    for (i = 0; i < MEM_SERVICE_ROCE_MAX_REGIONS; ++i) {
        if (!context->regions[i].in_use) {
            slot = &context->regions[i];
            break;
        }
    }
    if (slot == NULL) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    slot->mr = ibv_reg_mr(context->pd,
                          request->base,
                          (size_t)request->len,
                          IBV_ACCESS_LOCAL_WRITE |
                              IBV_ACCESS_REMOTE_READ |
                              IBV_ACCESS_REMOTE_WRITE);
    if (slot->mr == NULL) {
        return -1;
    }
    context->next_region_handle += 1U;
    slot->in_use = true;
    slot->handle = context->next_region_handle;
    slot->base = request->base;
    slot->len = request->len;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = htonl(MEM_SERVICE_ROCE_DESCRIPTOR_MAGIC);
    descriptor.version = htonl(MEM_SERVICE_ROCE_PROTOCOL_VERSION);
    descriptor.rkey = htonl(slot->mr->rkey);
    descriptor.address = mem_service_roce_hton64((uintptr_t)slot->base);
    descriptor.len = mem_service_roce_hton64(slot->len);
    memset(region_out, 0, sizeof(*region_out));
    region_out->handle = slot->handle;
    region_out->len = slot->len;
    region_out->memory_kind = request->memory_kind;
    region_out->descriptor.len = sizeof(descriptor);
    memcpy(region_out->descriptor.bytes, &descriptor, sizeof(descriptor));
    return 0;
}

static int mem_service_roce_provider_deregister_region(void *opaque,
                                                       uint64_t region_handle)
{
    struct mem_service_roce_context *context = opaque;
    struct mem_service_roce_region_slot *slot;

    if (context == NULL ||
        (slot = mem_service_roce_find_region(context, region_handle)) == NULL ||
        ibv_dereg_mr(slot->mr) != 0) {
        return -1;
    }
    memset(slot, 0, sizeof(*slot));
    return 0;
}

static int mem_service_roce_decode_descriptor(
    const struct mem_service_provider_descriptor *opaque,
    struct mem_service_roce_descriptor_wire *descriptor)
{
    if (opaque == NULL || descriptor == NULL ||
        opaque->len != sizeof(*descriptor)) {
        return -1;
    }
    memcpy(descriptor, opaque->bytes, sizeof(*descriptor));
    if (ntohl(descriptor->magic) != MEM_SERVICE_ROCE_DESCRIPTOR_MAGIC ||
        ntohl(descriptor->version) != MEM_SERVICE_ROCE_PROTOCOL_VERSION) {
        return -1;
    }
    descriptor->rkey = ntohl(descriptor->rkey);
    descriptor->address = mem_service_roce_ntoh64(descriptor->address);
    descriptor->len = mem_service_roce_ntoh64(descriptor->len);
    return descriptor->len == 0 ? -1 : 0;
}

static int mem_service_roce_provider_submit_transfer(
    void *opaque,
    const struct mem_service_transfer_request *request,
    uint64_t *completion_id_out)
{
    struct mem_service_roce_context *context = opaque;
    struct mem_service_roce_region_slot *source;
    struct mem_service_roce_descriptor_wire destination;
    struct ibv_sge scatter;
    struct ibv_send_wr send;
    struct ibv_send_wr *bad = NULL;
    uint64_t checksum;

    if (context == NULL || !context->connected || request == NULL ||
        completion_id_out == NULL || context->pending_completion_id != 0 ||
        request->source.len == 0 ||
        request->source.len != request->destination.len ||
        (source = mem_service_roce_find_region(
             context, request->source.region_handle)) == NULL ||
        request->source.offset > source->len ||
        request->source.len > source->len - request->source.offset ||
        mem_service_roce_decode_descriptor(&request->destination.descriptor,
                                           &destination) != 0 ||
        request->destination.offset > destination.len ||
        request->destination.len >
            destination.len - request->destination.offset) {
        return -1;
    }
    checksum = mem_service_roce_checksum(
        (const uint8_t *)source->base + request->source.offset,
        request->source.len);
    if (request->expected_checksum != 0 &&
        request->expected_checksum != checksum) {
        return -1;
    }
    context->next_completion_id += 1U;
    memset(&scatter, 0, sizeof(scatter));
    scatter.addr = (uintptr_t)source->base + request->source.offset;
    scatter.length = (uint32_t)request->source.len;
    scatter.lkey = source->mr->lkey;
    memset(&send, 0, sizeof(send));
    send.wr_id =
        MEM_SERVICE_ROCE_TRANSFER_WR_ID_BASE | context->next_completion_id;
    send.sg_list = &scatter;
    send.num_sge = 1;
    send.opcode = IBV_WR_RDMA_WRITE;
    send.send_flags = IBV_SEND_SIGNALED;
    send.wr.rdma.remote_addr =
        destination.address + request->destination.offset;
    send.wr.rdma.rkey = destination.rkey;
    if (ibv_post_send(context->id->qp, &send, &bad) != 0) {
        return -1;
    }
    context->pending_completion_id = context->next_completion_id;
    context->pending_transfer_bytes = request->source.len;
    context->pending_transfer_checksum = checksum;
    *completion_id_out = context->next_completion_id;
    return 0;
}

static int mem_service_roce_provider_poll_completion(
    void *opaque,
    uint64_t completion_id,
    struct mem_service_transfer_completion *completion_out)
{
    struct mem_service_roce_context *context = opaque;
    struct ibv_wc completion;

    if (context == NULL || completion_out == NULL || completion_id == 0 ||
        completion_id != context->pending_completion_id ||
        mem_service_roce_wait_completion(
            context,
            MEM_SERVICE_ROCE_TRANSFER_WR_ID_BASE | completion_id,
            &completion) != 0) {
        return -1;
    }
    memset(completion_out, 0, sizeof(*completion_out));
    completion_out->id = completion_id;
    completion_out->status = 0;
    completion_out->transferred_bytes = context->pending_transfer_bytes;
    completion_out->checksum = context->pending_transfer_checksum;
    context->pending_completion_id = 0;
    context->pending_transfer_bytes = 0;
    context->pending_transfer_checksum = 0;
    return 0;
}

static const struct mem_service_provider_ops mem_service_roce_provider_ops = {
    .probe = mem_service_roce_provider_probe,
    .register_region = mem_service_roce_provider_register_region,
    .deregister_region = mem_service_roce_provider_deregister_region,
    .submit_transfer = mem_service_roce_provider_submit_transfer,
    .poll_completion = mem_service_roce_provider_poll_completion,
};

int mem_service_provider_roce_endpoint_open(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config,
    bool server)
{
    struct mem_service_roce_context *context;
    int rc;

    if (endpoint == NULL || endpoint->implementation != NULL ||
        config == NULL) {
        return -1;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return -1;
    }
    rc = server
        ? mem_service_roce_listen_server(context, config)
        : mem_service_roce_open_client(context, config);
    if (rc == 0 && server) {
        rc = mem_service_roce_accept_server(context, config);
    }
    if (rc != 0) {
        mem_service_roce_context_destroy(context);
        free(context);
        return -1;
    }
    endpoint->implementation = context;
    return 0;
}

int mem_service_provider_roce_endpoint_listen(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config)
{
    struct mem_service_roce_context *context;

    if (endpoint == NULL || endpoint->implementation != NULL ||
        config == NULL) {
        return -1;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return -1;
    }
    if (mem_service_roce_listen_server(context, config) != 0) {
        mem_service_roce_context_destroy(context);
        free(context);
        return -1;
    }
    endpoint->implementation = context;
    return 0;
}

int mem_service_provider_roce_endpoint_accept(
    struct mem_service_provider_roce_endpoint *endpoint,
    const struct mem_service_provider_roce_config *config)
{
    if (endpoint == NULL || endpoint->implementation == NULL ||
        config == NULL) {
        return -1;
    }
    return mem_service_roce_accept_server(endpoint->implementation, config);
}

int mem_service_provider_roce_endpoint_registration(
    struct mem_service_provider_roce_endpoint *endpoint,
    struct mem_service_provider_registration *registration_out)
{
    struct mem_service_roce_context *context;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        registration_out == NULL) {
        return -1;
    }
    context = endpoint->implementation;
    memset(registration_out, 0, sizeof(*registration_out));
    registration_out->name = "roce";
    registration_out->instance = context->instance;
    registration_out->capabilities =
        MEM_SERVICE_PROVIDER_CAP_REGION_REGISTRATION |
        MEM_SERVICE_PROVIDER_CAP_PEER_TRANSFER;
    registration_out->ops = &mem_service_roce_provider_ops;
    registration_out->context = context;
    return 0;
}

void mem_service_provider_roce_endpoint_close(
    struct mem_service_provider_roce_endpoint *endpoint)
{
    struct mem_service_roce_context *context;

    if (endpoint == NULL || endpoint->implementation == NULL) {
        return;
    }
    context = endpoint->implementation;
    mem_service_roce_context_destroy(context);
    free(context);
    endpoint->implementation = NULL;
}

static int mem_service_roce_send_control(
    struct mem_service_roce_context *context,
    enum mem_service_roce_control_type type,
    uint32_t status,
    uint64_t address,
    uint64_t len,
    uint64_t checksum,
    uint64_t iteration,
    uint32_t rkey)
{
    struct ibv_sge scatter;
    struct ibv_send_wr send;
    struct ibv_send_wr *bad = NULL;
    struct ibv_wc completion;

    memset(&context->control_send, 0, sizeof(context->control_send));
    context->control_send.magic = htonl(MEM_SERVICE_ROCE_CONTROL_MAGIC);
    context->control_send.version = htonl(MEM_SERVICE_ROCE_PROTOCOL_VERSION);
    context->control_send.type = htonl((uint32_t)type);
    context->control_send.status = htonl(status);
    context->control_send.address = mem_service_roce_hton64(address);
    context->control_send.len = mem_service_roce_hton64(len);
    context->control_send.checksum = mem_service_roce_hton64(checksum);
    context->control_send.iteration = mem_service_roce_hton64(iteration);
    context->control_send.rkey = htonl(rkey);
    memset(&scatter, 0, sizeof(scatter));
    scatter.addr = (uintptr_t)&context->control_send;
    scatter.length = sizeof(context->control_send);
    scatter.lkey = context->control_send_mr->lkey;
    memset(&send, 0, sizeof(send));
    send.wr_id = MEM_SERVICE_ROCE_CONTROL_SEND_WR_ID;
    send.sg_list = &scatter;
    send.num_sge = 1;
    send.opcode = IBV_WR_SEND;
    send.send_flags = IBV_SEND_SIGNALED;
    if (ibv_post_send(context->id->qp, &send, &bad) != 0) {
        return -1;
    }
    return mem_service_roce_wait_completion(
        context, MEM_SERVICE_ROCE_CONTROL_SEND_WR_ID, &completion);
}

static int mem_service_roce_receive_control(
    struct mem_service_roce_context *context,
    enum mem_service_roce_control_type expected,
    struct mem_service_roce_control_wire *message)
{
    struct ibv_wc completion;

    if (mem_service_roce_wait_completion(
            context, MEM_SERVICE_ROCE_CONTROL_RECV_WR_ID, &completion) != 0 ||
        completion.opcode != IBV_WC_RECV ||
        completion.byte_len != sizeof(context->control_recv)) {
        return -1;
    }
    *message = context->control_recv;
    if (mem_service_roce_post_control_receive(context) != 0 ||
        ntohl(message->magic) != MEM_SERVICE_ROCE_CONTROL_MAGIC ||
        ntohl(message->version) != MEM_SERVICE_ROCE_PROTOCOL_VERSION ||
        ntohl(message->type) != (uint32_t)expected) {
        return -1;
    }
    message->status = ntohl(message->status);
    message->address = mem_service_roce_ntoh64(message->address);
    message->len = mem_service_roce_ntoh64(message->len);
    message->checksum = mem_service_roce_ntoh64(message->checksum);
    message->iteration = mem_service_roce_ntoh64(message->iteration);
    message->rkey = ntohl(message->rkey);
    return 0;
}

static int mem_service_roce_send_region(
    struct mem_service_roce_context *context,
    const struct mem_service_region *region)
{
    struct mem_service_roce_descriptor_wire descriptor;

    if (region == NULL ||
        mem_service_roce_decode_descriptor(&region->descriptor,
                                           &descriptor) != 0) {
        return -1;
    }
    return mem_service_roce_send_control(context,
                                         MEM_SERVICE_ROCE_CONTROL_REGION,
                                         0,
                                         descriptor.address,
                                         descriptor.len,
                                         0,
                                         0,
                                         descriptor.rkey);
}

static int mem_service_roce_receive_region(
    struct mem_service_roce_context *context,
    struct mem_service_provider_slice *slice)
{
    struct mem_service_roce_control_wire message;
    struct mem_service_roce_descriptor_wire descriptor;

    if (slice == NULL ||
        mem_service_roce_receive_control(context,
                                         MEM_SERVICE_ROCE_CONTROL_REGION,
                                         &message) != 0 ||
        message.status != 0 || message.address == 0 || message.len == 0 ||
        message.rkey == 0) {
        return -1;
    }
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = htonl(MEM_SERVICE_ROCE_DESCRIPTOR_MAGIC);
    descriptor.version = htonl(MEM_SERVICE_ROCE_PROTOCOL_VERSION);
    descriptor.rkey = htonl(message.rkey);
    descriptor.address = mem_service_roce_hton64(message.address);
    descriptor.len = mem_service_roce_hton64(message.len);
    memset(slice, 0, sizeof(*slice));
    slice->len = message.len;
    slice->descriptor.len = sizeof(descriptor);
    memcpy(slice->descriptor.bytes, &descriptor, sizeof(descriptor));
    return 0;
}

static void mem_service_roce_fill_payload(uint8_t *payload,
                                          uint64_t len,
                                          uint64_t iteration)
{
    uint64_t i;

    for (i = 0; i < len; ++i) {
        payload[i] = (uint8_t)((i * 131U + iteration * 17U + 0x5aU) & 0xffU);
    }
}

static void mem_service_roce_fill_result(
    const struct mem_service_roce_context *context,
    uint64_t payload_bytes,
    uint64_t iterations,
    uint64_t checksum,
    uint64_t elapsed_us,
    const struct mem_service_provider_registry *registry,
    struct mem_service_provider_roce_canary_result *result)
{
    memset(result, 0, sizeof(*result));
    snprintf(result->device, sizeof(result->device), "%s", context->device);
    snprintf(result->local_ipv4,
             sizeof(result->local_ipv4),
             "%s",
             context->local_ipv4);
    snprintf(result->peer_ipv4,
             sizeof(result->peer_ipv4),
             "%s",
             context->peer_ipv4);
    result->payload_bytes = payload_bytes;
    result->iterations = iterations;
    result->checksum = checksum;
    result->elapsed_us = elapsed_us;
    result->data_plane_ready =
        mem_service_provider_registry_data_plane_ready(registry);
}

int mem_service_provider_roce_probe_device(const char *device,
                                           char *detail,
                                           size_t detail_len)
{
    struct ibv_device **devices;
    struct ibv_context *verbs = NULL;
    struct ibv_port_attr port;
    int device_count = 0;
    int rc = -1;
    int i;

    if (device == NULL || device[0] == '\0' ||
        detail == NULL || detail_len == 0) {
        return -1;
    }
    detail[0] = '\0';
    devices = ibv_get_device_list(&device_count);
    if (devices == NULL) {
        return -1;
    }
    for (i = 0; i < device_count; ++i) {
        if (strcmp(ibv_get_device_name(devices[i]), device) == 0) {
            verbs = ibv_open_device(devices[i]);
            break;
        }
    }
    if (verbs != NULL && ibv_query_port(verbs, 1, &port) == 0 &&
        port.state == IBV_PORT_ACTIVE &&
        port.link_layer == IBV_LINK_LAYER_ETHERNET) {
        snprintf(detail,
                 detail_len,
                 "device=%s port=1 state=active link_layer=ethernet "
                 "active_mtu=%u",
                 device,
                 mem_service_roce_mtu_bytes(port.active_mtu));
        rc = 0;
    }
    if (verbs != NULL) {
        ibv_close_device(verbs);
    }
    ibv_free_device_list(devices);
    return rc;
}

int mem_service_provider_roce_endpoint_verify(
    struct mem_service_provider_roce_endpoint *endpoint,
    bool server,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result)
{
    struct mem_service_roce_context *context;
    struct mem_service_provider_registry registry;
    struct mem_service_provider_registration registration;
    struct mem_service_region_request region_request;
    struct mem_service_region local_region;
    struct mem_service_provider_slice remote_region;
    struct mem_service_transfer_request transfer;
    struct mem_service_transfer_completion completion;
    struct mem_service_roce_control_wire message;
    const struct mem_service_provider *provider = NULL;
    uint8_t *payload = NULL;
    uint64_t completion_id;
    uint64_t checksum = 0;
    uint64_t start_us;
    uint64_t elapsed_us;
    uint32_t iteration;
    int rc = -1;

    if (endpoint == NULL || endpoint->implementation == NULL ||
        result == NULL || payload_bytes == 0 ||
        payload_bytes > UINT32_MAX || iterations == 0 ||
        posix_memalign((void **)&payload, 4096, (size_t)payload_bytes) != 0) {
        return -1;
    }
    context = endpoint->implementation;
    memset(&registry, 0, sizeof(registry));
    memset(&local_region, 0, sizeof(local_region));
    memset(&remote_region, 0, sizeof(remote_region));
    if (mem_service_provider_roce_endpoint_registration(
            endpoint, &registration) != 0 ||
        mem_service_provider_registry_init(&registry) != 0 ||
        mem_service_provider_registry_register(
            &registry, &registration) != 0 ||
        (provider = mem_service_provider_registry_find(
             &registry, "roce", context->instance)) == NULL) {
        rc = -2;
        goto done;
    }
    memset(&region_request, 0, sizeof(region_request));
    region_request.base = payload;
    region_request.len = payload_bytes;
    region_request.memory_kind = MEM_SERVICE_MEMORY_HOST;
    if (provider->ops->register_region(provider->context,
                                       &region_request,
                                       &local_region) != 0) {
        rc = -3;
        goto done;
    }
    if (server) {
        memset(payload, 0, (size_t)payload_bytes);
        if (mem_service_roce_send_region(context, &local_region) != 0) {
            rc = -4;
            goto done;
        }
    } else if (mem_service_roce_receive_region(context, &remote_region) != 0 ||
               remote_region.len < payload_bytes) {
        rc = -4;
        goto done;
    }
    start_us = mem_service_roce_now_us();
    for (iteration = 0; iteration < iterations; ++iteration) {
        if (server) {
            if (mem_service_roce_receive_control(
                    context,
                    MEM_SERVICE_ROCE_CONTROL_DONE,
                    &message) != 0 ||
                message.status != 0 || message.iteration != iteration ||
                message.len != payload_bytes) {
                rc = -5;
                goto done;
            }
            checksum = mem_service_roce_checksum(payload, payload_bytes);
            if (checksum != message.checksum) {
                (void)mem_service_roce_send_control(
                    context,
                    MEM_SERVICE_ROCE_CONTROL_ACK,
                    1,
                    0,
                    payload_bytes,
                    checksum,
                    iteration,
                    0);
                rc = -6;
                goto done;
            }
            context->transfer_verified = true;
            /*
             * ACK is the writer's permission to start the next iteration.
             * Clear before ACK; clearing while waiting for DONE can overwrite
             * an RDMA_WRITE that has already completed on the same RC QP.
             */
            memset(payload, 0, (size_t)payload_bytes);
            if (mem_service_roce_send_control(
                    context,
                    MEM_SERVICE_ROCE_CONTROL_ACK,
                    0,
                    0,
                    payload_bytes,
                    checksum,
                    iteration,
                    0) != 0) {
                rc = -7;
                goto done;
            }
        } else {
            mem_service_roce_fill_payload(payload,
                                          payload_bytes,
                                          iteration);
            checksum = mem_service_roce_checksum(payload, payload_bytes);
            memset(&transfer, 0, sizeof(transfer));
            transfer.source.region_handle = local_region.handle;
            transfer.source.len = payload_bytes;
            transfer.destination = remote_region;
            transfer.destination.len = payload_bytes;
            transfer.expected_checksum = checksum;
            if (provider->ops->submit_transfer(provider->context,
                                               &transfer,
                                               &completion_id) != 0 ||
                provider->ops->poll_completion(provider->context,
                                               completion_id,
                                               &completion) != 0 ||
                completion.status != 0 ||
                completion.transferred_bytes != payload_bytes ||
                completion.checksum != checksum) {
                rc = -5;
                goto done;
            }
            if (mem_service_roce_send_control(
                    context,
                    MEM_SERVICE_ROCE_CONTROL_DONE,
                    0,
                    0,
                    payload_bytes,
                    checksum,
                    iteration,
                    0) != 0) {
                rc = -6;
                goto done;
            }
            if (mem_service_roce_receive_control(
                    context,
                    MEM_SERVICE_ROCE_CONTROL_ACK,
                    &message) != 0) {
                rc = -7;
                goto done;
            }
            if (message.status != 0 ||
                message.iteration != iteration ||
                message.len != payload_bytes ||
                message.checksum != checksum) {
                rc = -8;
                goto done;
            }
            context->transfer_verified = true;
        }
        if (mem_service_provider_registry_refresh(&registry) != 0 ||
            !mem_service_provider_registry_data_plane_ready(&registry)) {
            rc = -9;
            goto done;
        }
    }
    elapsed_us = mem_service_roce_now_us() - start_us;
    mem_service_roce_fill_result(context,
                                 payload_bytes,
                                 iterations,
                                 checksum,
                                 elapsed_us,
                                 &registry,
                                 result);
    rc = result->data_plane_ready ? 0 : -1;

done:
    if (provider != NULL && local_region.handle != 0) {
        (void)provider->ops->deregister_region(provider->context,
                                                local_region.handle);
    }
    free(payload);
    return rc;
}

int mem_service_provider_roce_run_server_canary(
    const struct mem_service_provider_roce_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result)
{
    struct mem_service_provider_roce_endpoint endpoint = {0};
    int rc;

    if (mem_service_provider_roce_endpoint_open(
            &endpoint, config, true) != 0) {
        return -1;
    }
    rc = mem_service_provider_roce_endpoint_verify(&endpoint,
                                                   true,
                                                   payload_bytes,
                                                   iterations,
                                                   result);
    mem_service_provider_roce_endpoint_close(&endpoint);
    return rc;
}

int mem_service_provider_roce_run_client_canary(
    const struct mem_service_provider_roce_config *config,
    uint64_t payload_bytes,
    uint32_t iterations,
    struct mem_service_provider_roce_canary_result *result)
{
    struct mem_service_provider_roce_endpoint endpoint = {0};
    int rc;

    if (mem_service_provider_roce_endpoint_open(
            &endpoint, config, false) != 0) {
        return -1;
    }
    rc = mem_service_provider_roce_endpoint_verify(&endpoint,
                                                   false,
                                                   payload_bytes,
                                                   iterations,
                                                   result);
    mem_service_provider_roce_endpoint_close(&endpoint);
    return rc;
}

int mem_service_provider_roce_run_protocol_fixture(void)
{
    struct mem_service_provider_descriptor opaque;
    struct mem_service_roce_descriptor_wire descriptor;
    struct mem_service_roce_descriptor_wire decoded;

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.magic = htonl(MEM_SERVICE_ROCE_DESCRIPTOR_MAGIC);
    descriptor.version = htonl(MEM_SERVICE_ROCE_PROTOCOL_VERSION);
    descriptor.rkey = htonl(0x11223344U);
    descriptor.address = mem_service_roce_hton64(0x1020304050607080ULL);
    descriptor.len = mem_service_roce_hton64(4096);
    memset(&opaque, 0, sizeof(opaque));
    opaque.len = sizeof(descriptor);
    memcpy(opaque.bytes, &descriptor, sizeof(descriptor));
    if (mem_service_roce_decode_descriptor(&opaque, &decoded) != 0 ||
        decoded.rkey != 0x11223344U ||
        decoded.address != 0x1020304050607080ULL ||
        decoded.len != 4096) {
        return 1;
    }
    opaque.bytes[0] ^= 0xffU;
    if (mem_service_roce_decode_descriptor(&opaque, &decoded) == 0) {
        return 1;
    }
    printf("mem_service roce-provider-fixtures: status=ok "
           "protocol_version=%u descriptor_bytes=%zu "
           "corruption=fail-closed\n",
           MEM_SERVICE_ROCE_PROTOCOL_VERSION,
           sizeof(descriptor));
    return 0;
}
