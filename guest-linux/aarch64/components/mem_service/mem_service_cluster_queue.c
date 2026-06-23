#include "mem_service_internal.h"

#include "mem_service_cluster_queue.h"
#include "mem_service_cluster_runtime.h"

static bool mem_service_desc_matches_barrier(const struct obmm_desc *desc,
                                       uint16_t desc_type,
                                       uint16_t epoch)
{
    return desc && desc->type == desc_type && (uint16_t)desc->cookie == epoch;
}

static bool mem_service_take_pending_barrier_desc(struct mem_service_cluster_runtime *rt,
                                            int owner_idx,
                                            uint16_t desc_type,
                                            uint16_t epoch)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_desc_matches_barrier(&rt->pending_descs[owner_idx][i],
                                       desc_type,
                                       epoch)) {
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

static bool mem_service_pending_object_desc_matches(const struct obmm_desc *desc,
                                              uint16_t epoch,
                                              const struct mem_service_record *record,
                                              uint32_t kind)
{
    uint32_t checksum_cookie;

    if (!desc || !record || desc->type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT ||
        (uint16_t)(desc->seq >> 48) != epoch || desc->flags != kind ||
        desc->payload_offset != record->object_backing_offset ||
        desc->payload_len != record->object_backing_len) {
        return false;
    }
    checksum_cookie = (uint32_t)(record->object_payload_checksum ^
                                 (record->object_payload_checksum >> 32));
    return desc->cookie == checksum_cookie;
}

bool mem_service_runtime_range_input_desc_matches(const struct obmm_desc *desc,
                                                  uint16_t epoch)
{
    uint64_t decode_step = epoch > 0 ? (uint64_t)epoch - 1ULL : 0ULL;
    uint64_t expected_len = mem_service_qwen3_handoff_hidden_bytes(decode_step);

    if (!desc || desc->type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT ||
        desc->flags != MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT ||
        desc->payload_len != expected_len) {
        return false;
    }
    return (uint16_t)(desc->seq >> 48) == epoch;
}

bool mem_service_qwen3_token_result_desc_matches(const struct obmm_desc *desc,
                                                 uint16_t epoch)
{
    if (!desc || desc->type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT ||
        desc->flags != MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT ||
        desc->payload_len != MEM_SERVICE_OBMM_QWEN3_TOKEN_RESULT_BYTES) {
        return false;
    }
    return (uint16_t)(desc->seq >> 48) == epoch;
}

bool mem_service_qwen3_object_desc_matches(const struct obmm_desc *desc,
                                           uint16_t epoch,
                                           uint32_t payload_kind,
                                           uint64_t min_payload_len,
                                           uint64_t max_payload_len)
{
    if (!desc || desc->type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT ||
        desc->flags != payload_kind ||
        desc->payload_len < min_payload_len ||
        desc->payload_len > max_payload_len) {
        return false;
    }
    return (uint16_t)(desc->seq >> 48) == epoch;
}

bool mem_service_qwen3_object_desc_kind_len_matches(
    const struct obmm_desc *desc,
    uint32_t payload_kind,
    uint64_t min_payload_len,
    uint64_t max_payload_len)
{
    return desc && desc->type == OBMM_DESC_MEM_SERVICE_OBJECT_PUT &&
           desc->flags == payload_kind &&
           desc->payload_len >= min_payload_len &&
           desc->payload_len <= max_payload_len;
}

bool mem_service_take_pending_runtime_range_input_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    struct obmm_desc *desc_out)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_runtime_range_input_desc_matches(&rt->pending_descs[owner_idx][i],
                                                   epoch)) {
            if (desc_out) {
                *desc_out = rt->pending_descs[owner_idx][i];
            }
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

bool mem_service_take_pending_qwen3_token_result_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    struct obmm_desc *desc_out)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_qwen3_token_result_desc_matches(&rt->pending_descs[owner_idx][i],
                                                  epoch)) {
            if (desc_out) {
                *desc_out = rt->pending_descs[owner_idx][i];
            }
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

bool mem_service_take_pending_qwen3_object_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint16_t epoch,
    uint32_t payload_kind,
    uint64_t min_payload_len,
    uint64_t max_payload_len,
    struct obmm_desc *desc_out)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_qwen3_object_desc_matches(&rt->pending_descs[owner_idx][i],
                                            epoch,
                                            payload_kind,
                                            min_payload_len,
                                            max_payload_len)) {
            if (desc_out) {
                *desc_out = rt->pending_descs[owner_idx][i];
            }
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

bool mem_service_take_pending_qwen3_object_kind_len_desc(
    struct mem_service_cluster_runtime *rt,
    int owner_idx,
    uint32_t payload_kind,
    uint64_t min_payload_len,
    uint64_t max_payload_len,
    struct obmm_desc *desc_out)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_qwen3_object_desc_kind_len_matches(
                &rt->pending_descs[owner_idx][i],
                payload_kind,
                min_payload_len,
                max_payload_len)) {
            if (desc_out) {
                *desc_out = rt->pending_descs[owner_idx][i];
            }
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

static bool mem_service_take_pending_object_desc(struct mem_service_cluster_runtime *rt,
                                           int owner_idx,
                                           uint16_t epoch,
                                           const struct mem_service_record *record,
                                           uint32_t kind)
{
    uint8_t count;
    uint8_t i;

    if (!rt || owner_idx < 0 || owner_idx >= rt->node_count) {
        return false;
    }
    count = rt->pending_desc_count[owner_idx];
    for (i = 0; i < count; ++i) {
        if (mem_service_pending_object_desc_matches(&rt->pending_descs[owner_idx][i],
                                              epoch,
                                              record,
                                              kind)) {
            if (i + 1 < count) {
                memmove(&rt->pending_descs[owner_idx][i],
                        &rt->pending_descs[owner_idx][i + 1],
                        (size_t)(count - i - 1) * sizeof(struct obmm_desc));
            }
            rt->pending_desc_count[owner_idx] = (uint8_t)(count - 1);
            return true;
        }
    }
    return false;
}

void mem_service_stash_pending_desc(struct mem_service_cluster_runtime *rt,
                                    int owner_idx,
                                    const struct obmm_desc *desc)
{
    uint8_t count;

    if (!rt || !desc || owner_idx < 0 || owner_idx >= rt->node_count) {
        return;
    }
    count = rt->pending_desc_count[owner_idx];
    if (count >= MEM_SERVICE_CLUSTER_PENDING_DESC_DEPTH) {
        fprintf(stderr,
                "[mem_service] pending desc overflow owner=%d type=%u cookie=0x%x\n",
                owner_idx,
                desc->type,
                desc->cookie);
        return;
    }
    rt->pending_descs[owner_idx][count] = *desc;
    rt->pending_desc_count[owner_idx] = (uint8_t)(count + 1);
}

int mem_service_queue_barrier(struct mem_service_cluster_runtime *rt,
                              uint16_t desc_type,
                              uint16_t epoch,
                              uint32_t publish_seq)
{
    long deadline = obmm_now_ms() + MEM_SERVICE_CLUSTER_WAIT_MS;
    bool got[MEM_SERVICE_CLUSTER_MAX_NODES];
    struct obmm_desc desc;
    int i;

    memset(got, 0, sizeof(got));
    got[rt->local_idx] = true;

    memset(&desc, 0, sizeof(desc));
    desc.type = desc_type;
    desc.seq = (uint64_t)epoch | ((uint64_t)publish_seq << 16);
    desc.cookie = (uint32_t)epoch;

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx) continue;
        if (rt->egress_queues[i] == NULL) continue;
        while (obmm_spsc_push(rt->egress_queues[i], &desc) != 0) {
            if (obmm_now_ms() > deadline) {
                fprintf(stderr, "[mem_service] queue barrier push timeout type=%d peer=%d\n",
                        desc_type, i);
                return -1;
            }
            usleep(1000);
        }
    }

    while (obmm_now_ms() < deadline) {
        bool all = true;
        for (i = 0; i < rt->node_count; i++) {
            struct obmm_desc rx;
            if (got[i]) continue;
            if (rt->ingress_queues[i] == NULL) continue;
            if (mem_service_take_pending_barrier_desc(rt, i, desc_type, epoch)) {
                got[i] = true;
                continue;
            }
            while (obmm_spsc_pop(rt->ingress_queues[i], &rx) == 0) {
                if (mem_service_desc_matches_barrier(&rx, desc_type, epoch)) {
                    got[i] = true;
                } else {
                    mem_service_stash_pending_desc(rt, i, &rx);
                }
            }
            if (!got[i]) all = false;
        }
        if (all) return 0;
        usleep(1000);
    }

    fprintf(stderr, "[mem_service] queue barrier timeout type=%d missing:", desc_type);
    for (i = 0; i < rt->node_count; i++)
        if (!got[i]) fprintf(stderr, " %d", i);
    fprintf(stderr, "\n");
    return -1;
}

int mem_service_push_obmm_object_descs(struct mem_service_cluster_runtime *rt,
                                       uint32_t payload_kind,
                                       uint64_t payload_offset,
                                       uint64_t payload_len,
                                       uint64_t checksum,
                                       uint16_t epoch)
{
    long deadline = obmm_now_ms() + MEM_SERVICE_CLUSTER_WAIT_MS;
    struct obmm_desc desc;
    int i;

    if (!rt || payload_len > UINT32_MAX || payload_kind > UINT16_MAX) {
        return -1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
    desc.flags = (uint16_t)payload_kind;
    desc.seq = ((uint64_t)epoch << 48) |
               ((uint64_t)(rt->local_idx + 1) << 32) |
               (payload_offset & 0xffffffffULL);
    desc.region_id = payload_kind;
    desc.payload_len = (uint32_t)payload_len;
    desc.payload_offset = payload_offset;
    desc.cookie = (uint32_t)(checksum ^ (checksum >> 32));

    for (i = 0; i < rt->node_count; i++) {
        if (i == rt->local_idx || rt->egress_queues[i] == NULL) {
            continue;
        }
        while (obmm_spsc_push(rt->egress_queues[i], &desc) != 0) {
            if (obmm_now_ms() > deadline) {
                fprintf(stderr,
                        "[mem_service] object desc push timeout kind=%u peer=%d offset=%#" PRIx64 "\n",
                        payload_kind, i + 1, payload_offset);
                return -1;
            }
            usleep(1000);
        }
    }

    return 0;
}

int mem_service_push_obmm_object_desc_to(struct mem_service_cluster_runtime *rt,
                                         uint32_t target_node,
                                         uint32_t payload_kind,
                                         uint64_t payload_offset,
                                         uint64_t payload_len,
                                         uint64_t checksum,
                                         uint16_t epoch)
{
    long deadline = obmm_now_ms() + MEM_SERVICE_CLUSTER_WAIT_MS;
    struct obmm_desc desc;

    if (!rt || target_node >= (uint32_t)rt->node_count ||
        target_node == (uint32_t)rt->local_idx ||
        payload_len > UINT32_MAX || payload_kind > UINT16_MAX) {
        return -1;
    }
    if (!rt->egress_queues[target_node] &&
        mem_service_activate_remote_slot(rt, (int)target_node) != 0) {
        return -1;
    }
    if (!rt->egress_queues[target_node]) {
        return -1;
    }

    memset(&desc, 0, sizeof(desc));
    desc.type = OBMM_DESC_MEM_SERVICE_OBJECT_PUT;
    desc.flags = (uint16_t)payload_kind;
    desc.seq = ((uint64_t)epoch << 48) |
               ((uint64_t)(rt->local_idx + 1) << 32) |
               (payload_offset & 0xffffffffULL);
    desc.region_id = payload_kind;
    desc.payload_len = (uint32_t)payload_len;
    desc.payload_offset = payload_offset;
    desc.cookie = (uint32_t)(checksum ^ (checksum >> 32));

    while (obmm_spsc_push(rt->egress_queues[target_node], &desc) != 0) {
        if (obmm_now_ms() > deadline) {
            fprintf(stderr,
                    "[mem_service] object desc unicast timeout kind=%u target=%u offset=%#" PRIx64 "\n",
                    payload_kind,
                    target_node + 1U,
                    payload_offset);
            return -1;
        }
        usleep(1000);
    }
    return 0;
}

int mem_service_wait_remote_obmm_object_descs(struct mem_service_cluster_runtime *rt,
                                             uint32_t owner_node,
                                             uint16_t epoch,
                                             const struct mem_service_record *weight,
                                             const struct mem_service_record *kvcache,
                                             const struct mem_service_record *hidden_input,
                                             const struct mem_service_record *hidden_output)
{
    long deadline = obmm_now_ms() + MEM_SERVICE_OBMM_SERVICE_WAIT_MS;
    bool saw_weight = false;
    bool saw_kvcache = false;
    bool saw_hidden_input = false;
    bool saw_hidden_output = false;
    struct obmm_spsc_queue *q;

    if (!rt || owner_node >= (uint32_t)rt->node_count || !weight || !kvcache ||
        !hidden_input || !hidden_output) {
        return -1;
    }
    q = rt->ingress_queues[owner_node];
    if (!q) {
        return -1;
    }

    while (obmm_now_ms() < deadline) {
        struct obmm_desc desc;
        bool drained = false;

        if (!saw_weight &&
            mem_service_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           weight,
                                           MEM_SERVICE_OBMM_KIND_WEIGHT_TILE)) {
            saw_weight = true;
        }
        if (!saw_kvcache &&
            mem_service_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           kvcache,
                                           MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK)) {
            saw_kvcache = true;
        }
        if (!saw_hidden_input &&
            mem_service_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           hidden_input,
                                           MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT)) {
            saw_hidden_input = true;
        }
        if (!saw_hidden_output &&
            mem_service_take_pending_object_desc(rt,
                                           (int)owner_node,
                                           epoch,
                                           hidden_output,
                                           MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT)) {
            saw_hidden_output = true;
        }
        while (obmm_spsc_pop(q, &desc) == 0) {
            bool matched = false;
            drained = true;
            if (desc.type != OBMM_DESC_MEM_SERVICE_OBJECT_PUT ||
                (uint16_t)(desc.seq >> 48) != epoch) {
                mem_service_stash_pending_desc(rt, (int)owner_node, &desc);
                continue;
            }
            if (desc.flags == MEM_SERVICE_OBMM_KIND_WEIGHT_TILE &&
                desc.payload_offset == weight->object_backing_offset &&
                desc.payload_len == weight->object_backing_len &&
                desc.cookie == (uint32_t)(weight->object_payload_checksum ^
                                          (weight->object_payload_checksum >> 32))) {
                saw_weight = true;
                matched = true;
            }
            if (desc.flags == MEM_SERVICE_OBMM_KIND_KVCACHE_BLOCK &&
                desc.payload_offset == kvcache->object_backing_offset &&
                desc.payload_len == kvcache->object_backing_len &&
                desc.cookie == (uint32_t)(kvcache->object_payload_checksum ^
                                          (kvcache->object_payload_checksum >> 32))) {
                saw_kvcache = true;
                matched = true;
            }
            if (desc.flags == MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_INPUT &&
                desc.payload_offset == hidden_input->object_backing_offset &&
                desc.payload_len == hidden_input->object_backing_len &&
                desc.cookie == (uint32_t)(hidden_input->object_payload_checksum ^
                                          (hidden_input->object_payload_checksum >> 32))) {
                saw_hidden_input = true;
                matched = true;
            }
            if (desc.flags == MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_OUTPUT &&
                desc.payload_offset == hidden_output->object_backing_offset &&
                desc.payload_len == hidden_output->object_backing_len &&
                desc.cookie == (uint32_t)(hidden_output->object_payload_checksum ^
                                          (hidden_output->object_payload_checksum >> 32))) {
                saw_hidden_output = true;
                matched = true;
            }
            if (!matched) {
                mem_service_stash_pending_desc(rt, (int)owner_node, &desc);
            }
        }
        if (saw_weight && saw_kvcache && saw_hidden_input && saw_hidden_output) {
            return 0;
        }
        if (!drained) {
            usleep(1000);
        }
    }

    printf("[mem_service] gap obmm_service_v0=object_desc_timeout remote=node%u weight=%u kvcache=%u hidden_input=%u hidden_output=%u epoch=%u\n",
           owner_node + 1U,
           saw_weight ? 1U : 0U,
           saw_kvcache ? 1U : 0U,
           saw_hidden_input ? 1U : 0U,
           saw_hidden_output ? 1U : 0U,
           epoch);
    return -1;
}
