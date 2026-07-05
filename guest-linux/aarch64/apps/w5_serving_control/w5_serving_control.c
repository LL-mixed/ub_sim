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
#include <unistd.h>

#define W5_SERVING_CONTROL_MAX_LINE 1024U
#define W5_SERVING_CONTROL_TIMEOUT_MS 90000L

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

static bool request_desc_matches(const struct obmm_desc *desc)
{
    return desc && desc->type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT &&
           desc->flags == MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST &&
           desc->payload_len > 0 &&
           desc->payload_len <= W5_SERVING_CONTROL_MAX_LINE;
}

static bool ack_desc_matches(const struct obmm_desc *desc,
                             const struct obmm_desc *request)
{
    return desc && request && desc->type == OBMM_DESC_MEM_SERVICE_OBJECT_GET &&
           desc->flags == MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST &&
           desc->payload_offset == request->payload_offset &&
           desc->payload_len == request->payload_len &&
           desc->cookie == request->cookie;
}

static int wait_for_worker_acks(struct mem_service_cluster_runtime *rt,
                                const struct obmm_desc *request)
{
    bool got[MEM_SERVICE_CLUSTER_MAX_NODES];
    long deadline = mem_service_wallclock_ms() + W5_SERVING_CONTROL_TIMEOUT_MS;
    int i;

    memset(got, 0, sizeof(got));
    got[rt->local_idx] = true;
    while (mem_service_wallclock_ms() < deadline) {
        bool all = true;

        for (i = 0; i < rt->node_count; ++i) {
            struct obmm_desc rx;

            if (got[i]) {
                continue;
            }
            if (!rt->ingress_queues[i]) {
                all = false;
                continue;
            }
            while (obmm_spsc_pop(rt->ingress_queues[i], &rx) == 0) {
                if (ack_desc_matches(&rx, request)) {
                    got[i] = true;
                    break;
                }
                mem_service_stash_pending_desc(rt, i, &rx);
            }
            if (!got[i]) {
                all = false;
            }
        }
        if (all) {
            return 0;
        }
        usleep(1000);
    }
    fprintf(stderr, "linqu_w5_serving_control: request ack timeout missing:");
    for (i = 0; i < rt->node_count; ++i) {
        if (!got[i]) {
            fprintf(stderr, " node%d", i + 1);
        }
    }
    fputc('\n', stderr);
    return -1;
}

static int publish_request_line(int argc, char **argv)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    struct mem_service_cluster_slot *local_slot;
    struct obmm_desc desc;
    const char *line = arg_value(argc, argv, "--request-line");
    uint64_t offset = 0;
    size_t len;
    uint64_t checksum;
    int rc;

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
    local_slot = &rt->slots[rt->local_idx];
    if (mem_service_payload_arena_alloc(rt, len, 64, &offset) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    memcpy((uint8_t *)local_slot->region.addr + offset, line, len);
    if (mem_service_update_region_range_at(local_slot, offset, len, true) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    checksum = w5_serving_checksum((const uint8_t *)line, len);
    if (mem_service_push_obmm_object_descs(rt,
                                           MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST,
                                           offset,
                                           len,
                                           checksum,
                                           1U) != 0) {
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    memset(&desc, 0, sizeof(desc));
    desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
    desc.flags = MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST;
    desc.payload_offset = offset;
    desc.payload_len = (uint32_t)len;
    desc.cookie = (uint32_t)(checksum ^ (checksum >> 32));
    fprintf(stderr,
            "linqu_w5_serving_control: published request bytes=%zu offset=0x%016" PRIx64
            " checksum=0x%016" PRIx64 " targets=%d\n",
            len,
            offset,
            checksum,
            rt->node_count - 1);
    rc = wait_for_worker_acks(rt, &desc);
    mem_service_cluster_runtime_destroy(rt);
    return rc;
}

static int send_ack(struct mem_service_cluster_runtime *rt,
                    int source_node,
                    const struct obmm_desc *request)
{
    struct obmm_desc ack;
    long deadline = mem_service_wallclock_ms() + W5_SERVING_CONTROL_TIMEOUT_MS;

    if (!rt->egress_queues[source_node] &&
        mem_service_activate_remote_slot(rt, source_node) != 0) {
        return -1;
    }
    memset(&ack, 0, sizeof(ack));
    ack.type = OBMM_DESC_MEM_SERVICE_OBJECT_GET;
    ack.flags = MEM_SERVICE_OBMM_KIND_W5_SERVING_REQUEST;
    ack.payload_offset = request->payload_offset;
    ack.payload_len = request->payload_len;
    ack.cookie = request->cookie;
    while (obmm_spsc_push(rt->egress_queues[source_node], &ack) != 0) {
        if (mem_service_wallclock_ms() > deadline) {
            fprintf(stderr,
                    "linqu_w5_serving_control: ack push timeout source=node%d\n",
                    source_node + 1);
            return -1;
        }
        usleep(1000);
    }
    return 0;
}

static int wait_request_line(int argc, char **argv)
{
    struct mem_service_cluster_runtime *rt = mem_service_cluster_runtime_current();
    const char *source_text = arg_value(argc, argv, "--source-node");
    const char *out_path = arg_value(argc, argv, "--out");
    int source_node = parse_source_node(source_text);
    long deadline = mem_service_wallclock_ms() + W5_SERVING_CONTROL_TIMEOUT_MS;
    struct obmm_desc desc;

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
    memset(&desc, 0, sizeof(desc));
    while (mem_service_wallclock_ms() < deadline) {
        struct obmm_desc rx;

        if (!rt->ingress_queues[source_node]) {
            usleep(1000);
            continue;
        }
        while (obmm_spsc_pop(rt->ingress_queues[source_node], &rx) == 0) {
            if (request_desc_matches(&rx)) {
                desc = rx;
                break;
            }
            mem_service_stash_pending_desc(rt, source_node, &rx);
        }
        if (request_desc_matches(&desc)) {
            break;
        }
        usleep(1000);
    }
    if (!request_desc_matches(&desc)) {
        fprintf(stderr,
                "linqu_w5_serving_control: request wait timeout source=node%d\n",
                source_node + 1);
        mem_service_cluster_runtime_destroy(rt);
        return 1;
    }
    {
        struct mem_service_cluster_slot *source_slot = &rt->slots[source_node];
        uint8_t bytes[W5_SERVING_CONTROL_MAX_LINE];
        uint64_t checksum;

        if (desc.payload_offset + desc.payload_len > source_slot->region.len) {
            fprintf(stderr,
                    "linqu_w5_serving_control: request payload out of range\n");
            mem_service_cluster_runtime_destroy(rt);
            return 1;
        }
        if (mem_service_sync_remote_range(source_slot,
                                          desc.payload_offset,
                                          desc.payload_len) != 0) {
            mem_service_cluster_runtime_destroy(rt);
            return 1;
        }
        memcpy(bytes,
               (const uint8_t *)source_slot->region.addr + desc.payload_offset,
               desc.payload_len);
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
                "linqu_w5_serving_control: received request bytes=%u source=node%d\n",
                desc.payload_len,
                source_node + 1);
    }
    mem_service_cluster_runtime_destroy(rt);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
                "usage: linqu_w5_serving_control publish --request-line LINE | wait --source-node nodeA [--out FILE]\n");
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
