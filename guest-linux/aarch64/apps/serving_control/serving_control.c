#include "components/mem_service/mem_service_cluster_queue.h"
#include "components/mem_service/mem_service_cluster_runtime.h"
#include "components/mem_service/mem_service_cluster_utils.h"
#include "components/mem_service/mem_service_object_contract.h"
#include "components/mem_service/mem_service_obmm_objects.h"
#include "libs/obmm_queue/obmm_spsc_queue.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define W5_SERVING_CONTROL_MAX_LINE 1024U
#define W5_SERVING_CONTROL_SLOT_BYTES 2048ULL
#define W5_SERVING_CONTROL_MAX_REQUESTS 4096U
#define W5_SERVING_CONTROL_REFRESH_MS 250L
#define W5_SERVING_CONTROL_TIMEOUT_MS 90000L
#define W5_SERVING_CONTROL_REQUEST_MAGIC 0x57355251U
#define W5_SERVING_CONTROL_ACK_MAGIC 0x57354143U

struct w5_serving_control_slot {
    uint32_t magic;
    uint32_t request_index;
    uint32_t payload_len;
    uint32_t reserved;
    uint64_t checksum;
    char payload[W5_SERVING_CONTROL_MAX_LINE];
};

static uint64_t w5_serving_checksum(const uint8_t *bytes, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;

    for (i = 0; i < len; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static const char *arg_value(int argc, char **argv, const char *name)
{
    int i;

    for (i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static int parse_request_index(int argc, char **argv, uint32_t *index_out)
{
    const char *raw = arg_value(argc, argv, "--request-index");
    char *end = NULL;
    unsigned long value;

    if (!index_out) {
        return -1;
    }
    if (!raw || raw[0] == '\0') {
        *index_out = 0;
        return 0;
    }
    errno = 0;
    value = strtoul(raw, &end, 10);
    if (errno != 0 || end == raw || *end != '\0' ||
        value >= W5_SERVING_CONTROL_MAX_REQUESTS) {
        fprintf(stderr,
                "linqu_w5_serving_control: invalid --request-index %s\n",
                raw);
        return -1;
    }
    *index_out = (uint32_t)value;
    return 0;
}

static int parse_source_node(const char *text)
{
    if (!text || strcmp(text, "nodeA") == 0 || strcmp(text, "0") == 0) {
        return 0;
    }
    if (strncmp(text, "node", 4U) == 0 && text[4] >= 'A' && text[4] <= 'H' &&
        text[5] == '\0') {
        return text[4] - 'A';
    }
    if (text[0] >= '1' && text[0] <= '8' && text[1] == '\0') {
        return text[0] - '1';
    }
    return -1;
}

static int activate_all_peers(struct mem_service_cluster_runtime *rt)
{
    int i;

    for (i = 0; i < rt->node_count; ++i) {
        if (i == rt->local_idx) {
            continue;
        }
        if (mem_service_activate_remote_slot(rt, i) != 0) {
            fprintf(stderr,
                    "linqu_w5_serving_control: activate peer failed node=%d\n",
                    i + 1);
            return -1;
        }
    }
    return 0;
}

static uint64_t request_slot_offset(uint32_t request_index)
{
    return MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET +
           (uint64_t)request_index * W5_SERVING_CONTROL_SLOT_BYTES;
}

static uint64_t ack_slot_offset(uint32_t request_index)
{
    return MEM_SERVICE_OBMM_QWEN3_DYNAMIC_ARENA_OFFSET +
           (uint64_t)W5_SERVING_CONTROL_MAX_REQUESTS *
               W5_SERVING_CONTROL_SLOT_BYTES +
           (uint64_t)request_index * W5_SERVING_CONTROL_SLOT_BYTES;
}

static uint64_t checksum_from_desc(const struct obmm_desc *desc)
{
    uint32_t high;
    uint32_t low;

    if (!desc) {
        return 0;
    }
    high = desc->region_id;
    low = desc->cookie ^ high;
    return ((uint64_t)high << 32) | (uint64_t)low;
}

static int write_local_control_slot(struct mem_service_cluster_runtime *rt,
                                    uint64_t offset,
                                    uint32_t magic,
                                    uint32_t request_index,
                                    const char *payload,
                                    size_t payload_len,
                                    uint64_t checksum)
{
    struct mem_service_cluster_slot *local_slot;
    struct w5_serving_control_slot *slot;

    if (!rt || rt->local_idx < 0 || !payload ||
        payload_len > W5_SERVING_CONTROL_MAX_LINE) {
        return -1;
    }
    local_slot = &rt->slots[rt->local_idx];
    if (!local_slot->region.addr ||
        offset + sizeof(*slot) < offset ||
        offset + sizeof(*slot) > local_slot->region.len) {
        return -1;
    }
    slot = (struct w5_serving_control_slot *)((uint8_t *)local_slot->region.addr +
                                              offset);
    memset(slot, 0, sizeof(*slot));
    if (mem_service_update_region_range_at(local_slot, offset, sizeof(*slot), true) != 0) {
        return -1;
    }
    (void)msync((uint8_t *)local_slot->region.addr + offset, sizeof(*slot), MS_SYNC);
    slot->request_index = request_index;
    slot->payload_len = (uint32_t)payload_len;
    slot->checksum = checksum;
    memcpy(slot->payload, payload, payload_len);
    slot->magic = magic;
    if (mem_service_update_region_range_at(local_slot, offset, sizeof(*slot), true) != 0) {
        return -1;
    }
    (void)msync((uint8_t *)local_slot->region.addr + offset, sizeof(*slot), MS_SYNC);
    return 0;
}

static bool read_remote_control_slot(struct mem_service_cluster_runtime *rt,
                                     int owner_idx,
                                     uint64_t offset,
                                     uint32_t magic,
                                     uint32_t request_index,
                                     struct w5_serving_control_slot *slot_out)
{
    struct mem_service_cluster_slot *owner_slot;
    struct w5_serving_control_slot slot;

    if (!rt || !slot_out || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    owner_slot = &rt->slots[owner_idx];
    if (!owner_slot->region.addr ||
        offset + sizeof(slot) < offset ||
        offset + sizeof(slot) > owner_slot->region.len) {
        return false;
    }
    if (mem_service_sync_remote_range(owner_slot, offset, sizeof(slot)) != 0) {
        return false;
    }
    memcpy(&slot, (const uint8_t *)owner_slot->region.addr + offset, sizeof(slot));
    if (slot.magic != magic || slot.request_index != request_index ||
        slot.payload_len == 0 ||
        slot.payload_len > W5_SERVING_CONTROL_MAX_LINE) {
        return false;
    }
    *slot_out = slot;
    return true;
}

static bool request_desc_matches(const struct obmm_desc *desc, uint32_t request_index)
{
    return desc && desc->type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT &&
           desc->flags == MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST &&
           (uint16_t)(desc->seq >> 48) == (uint16_t)(request_index + 1U) &&
           desc->payload_len > 0 &&
           desc->payload_len <= W5_SERVING_CONTROL_MAX_LINE;
}

static int publish_request_line(int argc, char **argv)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    const char *line = arg_value(argc, argv, "--request-line");
    uint64_t offset = 0;
    size_t len;
    uint64_t checksum;
    uint32_t request_index;

    if (parse_request_index(argc, argv, &request_index) != 0) {
        return 2;
    }
    if (!line || line[0] == '\0') {
        fprintf(stderr, "linqu_w5_serving_control: --request-line is required\n");
        return 2;
    }
    len = strlen(line) + 1U;
    if (len > W5_SERVING_CONTROL_MAX_LINE) {
        fprintf(stderr,
                "linqu_w5_serving_control: request line too long bytes=%zu max=%u\n",
                len,
                W5_SERVING_CONTROL_MAX_LINE);
        return 2;
    }
    if (mem_service_cluster_runtime_init(rt) != 0 ||
        mem_service_cluster_runtime_require(rt) != 0) {
        fprintf(stderr, "linqu_w5_serving_control: cluster runtime unavailable\n");
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    if (rt->local_idx != 0) {
        fprintf(stderr,
                "linqu_w5_serving_control: publish must run on nodeA local=node%d\n",
                rt->local_idx + 1);
        mem_service_cluster_runtime_destroy(rt);
        return 2;
    }
    if (activate_all_peers(rt) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    checksum = w5_serving_checksum((const uint8_t *)line, len);
    offset = request_slot_offset(request_index);
    if (write_local_control_slot(rt,
                                 offset,
                                 W5_SERVING_CONTROL_REQUEST_MAGIC,
                                 request_index,
                                 line,
                                 len,
                                 checksum) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    fprintf(stderr,
            "linqu_w5_serving_control: published request bytes=%zu offset=0x%016" PRIx64
            " checksum=0x%016" PRIx64 " index=%u targets=%d\n",
            len,
            offset,
            checksum,
            request_index,
            rt->node_count - 1);
    mem_service_cluster_runtime_destroy(rt);
    return 0;
}

static int send_ack(struct mem_service_cluster_runtime *rt,
                    int source_node,
                    const struct obmm_desc *request)
{
    uint32_t request_index = (uint32_t)((request->seq >> 48) - 1U);
    uint64_t checksum = checksum_from_desc(request);
    char ack_payload[32];

    (void)source_node;
    snprintf(ack_payload, sizeof(ack_payload), "ack:%u", request_index);
    return write_local_control_slot(rt,
                                    ack_slot_offset(request_index),
                                    W5_SERVING_CONTROL_ACK_MAGIC,
                                    request_index,
                                    ack_payload,
                                    strlen(ack_payload) + 1U,
                                    checksum);
}

static int wait_request_line(int argc, char **argv)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    const char *source_text = arg_value(argc, argv, "--source-node");
    const char *out_path = arg_value(argc, argv, "--out");
    int source_node = parse_source_node(source_text);
    long deadline = mem_service_wallclock_ms() + W5_SERVING_CONTROL_TIMEOUT_MS;
    struct obmm_desc desc;
    long next_refresh;
    uint32_t request_index;
    struct w5_serving_control_slot accepted_slot;

    if (parse_request_index(argc, argv, &request_index) != 0) {
        return 2;
    }
    if (source_node < 0) {
        fprintf(stderr, "linqu_w5_serving_control: invalid --source-node\n");
        return 2;
    }
    if (mem_service_cluster_runtime_init(rt) != 0 ||
        mem_service_cluster_runtime_require(rt) != 0) {
        fprintf(stderr, "linqu_w5_serving_control: cluster runtime unavailable\n");
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    if (source_node == rt->local_idx) {
        fprintf(stderr, "linqu_w5_serving_control: source cannot be local node\n");
        mem_service_cluster_runtime_destroy(rt);
        return 2;
    }
    if (mem_service_activate_remote_slot(rt, source_node) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    next_refresh = mem_service_wallclock_ms() + W5_SERVING_CONTROL_REFRESH_MS;
    memset(&desc, 0, sizeof(desc));
    memset(&accepted_slot, 0, sizeof(accepted_slot));
    while (mem_service_wallclock_ms() < deadline) {
        struct w5_serving_control_slot request_slot;
        long now = mem_service_wallclock_ms();

        if (now >= next_refresh) {
            (void)mem_service_refresh_remote_slot(rt, source_node);
            next_refresh = now + W5_SERVING_CONTROL_REFRESH_MS;
        }
        if (read_remote_control_slot(rt,
                                     source_node,
                                     request_slot_offset(request_index),
                                     W5_SERVING_CONTROL_REQUEST_MAGIC,
                                     request_index,
                                     &request_slot) &&
            w5_serving_checksum((const uint8_t *)request_slot.payload,
                                request_slot.payload_len) ==
                request_slot.checksum) {
            desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
            desc.flags = MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST;
            desc.seq = ((uint64_t)(request_index + 1U) << 48) |
                       ((uint64_t)(source_node + 1) << 32) |
                       (request_slot_offset(request_index) & 0xffffffffULL);
            desc.region_id = (uint32_t)(request_slot.checksum >> 32);
            desc.payload_offset = request_slot_offset(request_index);
            desc.payload_len = request_slot.payload_len;
            desc.cookie = (uint32_t)(request_slot.checksum ^
                                     (request_slot.checksum >> 32));
            accepted_slot = request_slot;
            break;
        }
        usleep(1000);
    }
    if (!request_desc_matches(&desc, request_index)) {
        fprintf(stderr,
                "linqu_w5_serving_control: request wait timeout source=node%d index=%u\n",
                source_node + 1,
                request_index);
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    {
        uint8_t bytes[W5_SERVING_CONTROL_MAX_LINE];
        uint64_t checksum;

        memcpy(bytes, accepted_slot.payload, desc.payload_len);
        bytes[desc.payload_len - 1U] = '\0';
        checksum = w5_serving_checksum(bytes, desc.payload_len);
        if ((uint32_t)(checksum ^ (checksum >> 32)) != desc.cookie) {
            fprintf(stderr,
                    "linqu_w5_serving_control: request checksum mismatch\n");
            mem_service_cluster_runtime_destroy(rt);
            return 1;
        }
        if (send_ack(rt, source_node, &desc) != 0) {
            mem_service_cluster_runtime_destroy(rt);
            return 1;
        }
        if (out_path && out_path[0] != '\0') {
            FILE *fp = fopen(out_path, "w");
            if (!fp) {
                fprintf(stderr,
                        "linqu_w5_serving_control: open output failed path=%s errno=%d\n",
                        out_path,
                        errno);
                mem_service_cluster_runtime_destroy(rt);
                return 1;
            }
            fprintf(fp, "%s\n", bytes);
            if (fclose(fp) != 0) {
                fprintf(stderr,
                        "linqu_w5_serving_control: close output failed path=%s errno=%d\n",
                        out_path,
                        errno);
                mem_service_cluster_runtime_destroy(rt);
                return 1;
            }
        } else {
            printf("%s\n", bytes);
            fflush(stdout);
        }
        fprintf(stderr,
                "linqu_w5_serving_control: received request bytes=%u source=node%d index=%u\n",
                desc.payload_len,
                source_node + 1,
                request_index);
    }
    mem_service_cluster_runtime_destroy(rt);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: linqu_w5_serving_control publish --request-line LINE [--request-index N] | wait --source-node nodeA [--request-index N] [--out FILE]\n");
        return 2;
    }
    if (strcmp(argv[1], "publish") == 0) {
        return publish_request_line(argc, argv);
    }
    if (strcmp(argv[1], "wait") == 0) {
        return wait_request_line(argc, argv);
    }
    fprintf(stderr, "linqu_w5_serving_control: unknown command %s\n", argv[1]);
    return 2;
}
