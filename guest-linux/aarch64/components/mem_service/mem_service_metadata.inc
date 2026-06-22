#include "mem_service.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int mem_service_put_request_prefix(struct mem_service *svc,
                                    const char *key,
                                    const char *request_id,
                                    const char *prefix_group,
                                    const char *group_id,
                                    const char *block_hash)
{
    struct mem_service_record *rec;

    rec = mem_service_find_record(svc, key);
    if (!rec) {
        rec = mem_service_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = MEM_SERVICE_RECORD_REQUEST_PREFIX;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->version = 1;
    return 0;
}

static int mem_service_put_prefix_group(struct mem_service *svc,
                                  const char *key,
                                  const char *request_id,
                                  const char *group_id,
                                  const char *block_hash,
                                  uint32_t placement_node,
                                  uint32_t placement_level,
                                  uint64_t hot_segment_id,
                                  enum mem_service_kvcache_state state,
                                  uint64_t last_result_segment)
{
    struct mem_service_record *rec;
    bool is_new = false;
    bool changed = false;

    rec = mem_service_find_record(svc, key);
    if (!rec) {
        rec = mem_service_alloc_record(svc);
        if (!rec) {
            return -1;
        }
        memset(rec, 0, sizeof(*rec));
        rec->in_use = true;
        rec->kind = MEM_SERVICE_RECORD_PREFIX_GROUP;
        snprintf(rec->key, sizeof(rec->key), "%s", key);
        snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
        snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
        rec->version = 1;
        is_new = true;
        changed = true;
    }
    if (rec->kind != MEM_SERVICE_RECORD_PREFIX_GROUP) {
        return -1;
    }
    if (mem_service_add_member(rec, block_hash) != 0) {
        return -1;
    }
    if (rec->placement_node != placement_node ||
        rec->placement_level != placement_level ||
        rec->hot_segment_id != hot_segment_id ||
        rec->state != state ||
        rec->last_result_segment != last_result_segment) {
        rec->placement_node = placement_node;
        rec->placement_level = placement_level;
        rec->hot_segment_id = hot_segment_id;
        rec->state = state;
        rec->last_result_segment = last_result_segment;
        changed = true;
    }
    if (changed && !is_new && rec->version > 0) {
        rec->version += 1;
    }
    return 0;
}

static int mem_service_put_block_meta(struct mem_service *svc,
                                const char *key,
                                const char *request_id,
                                const char *prefix_group,
                                const char *group_id,
                                const char *block_hash,
                                uint32_t placement_node,
                                uint32_t placement_level,
                                uint64_t hot_segment_id,
                                enum mem_service_kvcache_state state)
{
    struct mem_service_record *rec;

    rec = mem_service_find_record(svc, key);
    if (!rec) {
        rec = mem_service_alloc_record(svc);
    }
    if (!rec) {
        return -1;
    }

    memset(rec, 0, sizeof(*rec));
    rec->in_use = true;
    rec->kind = MEM_SERVICE_RECORD_BLOCK_META;
    snprintf(rec->key, sizeof(rec->key), "%s", key);
    snprintf(rec->request_id, sizeof(rec->request_id), "%s", request_id);
    snprintf(rec->prefix_group, sizeof(rec->prefix_group), "%s", prefix_group);
    snprintf(rec->group_id, sizeof(rec->group_id), "%s", group_id);
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_hash);
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->state = state;
    rec->version = 1;
    return 0;
}

static int mem_service_update_block_result(struct mem_service *svc,
                                     const char *key,
                                     uint64_t last_result_segment,
                                     enum mem_service_kvcache_state next_state)
{
    struct mem_service_record *rec = mem_service_find_record(svc, key);

    if (!rec || rec->kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->last_result_segment != 0 && last_result_segment <= rec->last_result_segment) {
        return 1;
    }
    rec->last_result_segment = last_result_segment;
    rec->state = next_state;
    rec->version += 1;
    return 0;
}

static int mem_service_update_prefix_result(struct mem_service *svc,
                                      const char *key,
                                      const struct mem_service_block_ctx *ctx,
                                      const struct mem_service_record *block_record)
{
    struct mem_service_record *rec = mem_service_find_record(svc, key);

    if (!rec || rec->kind != MEM_SERVICE_RECORD_REQUEST_PREFIX) {
        return -1;
    }
    if (rec->last_result_segment != 0 &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0 &&
        block_record->last_result_segment < rec->last_result_segment) {
        return 1;
    }
    if (rec->last_result_segment == block_record->last_result_segment &&
        rec->placement_node == ctx->placement_node &&
        rec->placement_level == ctx->placement_level &&
        rec->hot_segment_id == block_record->hot_segment_id &&
        rec->state == block_record->state &&
        strncmp(rec->block_hash, block_record->block_hash, sizeof(rec->block_hash)) == 0) {
        return 1;
    }
    rec->placement_node = block_record->placement_node;
    rec->placement_level = block_record->placement_level;
    rec->hot_segment_id = block_record->hot_segment_id;
    rec->state = block_record->state;
    rec->last_result_segment = block_record->last_result_segment;
    snprintf(rec->block_hash, sizeof(rec->block_hash), "%s", block_record->block_hash);
    rec->version += 1;
    return 0;
}

static int mem_service_update_prefix_group_from_block(struct mem_service *svc,
                                                const struct mem_service_block_ctx *ctx,
                                                const struct mem_service_record *block_record)
{
    char group_key[96];

    if (!svc || !ctx || !block_record) {
        return -1;
    }
    if (mem_service_build_group_key(ctx, group_key, sizeof(group_key)) != 0) {
        return -1;
    }
    return mem_service_put_prefix_group(svc,
                                  group_key,
                                  ctx->request_id,
                                  ctx->group_id,
                                  block_record->block_hash,
                                  block_record->placement_node,
                                  block_record->placement_level,
                                  block_record->hot_segment_id,
                                  block_record->state,
                                  block_record->last_result_segment);
}

static int mem_service_update_block_view(struct mem_service *svc,
                                   const char *key,
                                   uint64_t hot_segment_id,
                                   uint32_t placement_level)
{
    struct mem_service_record *rec = mem_service_find_record(svc, key);

    if (!rec || rec->kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->hot_segment_id == hot_segment_id && rec->placement_level == placement_level) {
        return 1;
    }
    rec->hot_segment_id = hot_segment_id;
    rec->placement_level = placement_level;
    rec->version += 1;
    return 0;
}

static int mem_service_update_block_owner(struct mem_service *svc,
                                    const char *key,
                                    uint32_t placement_node,
                                    uint32_t placement_level,
                                    uint64_t hot_segment_id)
{
    struct mem_service_record *rec = mem_service_find_record(svc, key);

    if (!rec || rec->kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return -1;
    }
    if (rec->placement_node == placement_node &&
        rec->placement_level == placement_level &&
        rec->hot_segment_id == hot_segment_id) {
        return 1;
    }
    rec->placement_node = placement_node;
    rec->placement_level = placement_level;
    rec->hot_segment_id = hot_segment_id;
    rec->version += 1;
    return 0;
}

bool mem_service_prefix_matches_block_meta(const struct mem_service_record *prefix_meta,
                                     const struct mem_service_record *block_meta)
{
    if (!prefix_meta || !block_meta) {
        return false;
    }
    if (prefix_meta->kind != MEM_SERVICE_RECORD_REQUEST_PREFIX ||
        block_meta->kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return false;
    }
    return prefix_meta->last_result_segment != 0 &&
           block_meta->last_result_segment != 0 &&
           strncmp(prefix_meta->block_hash, block_meta->block_hash,
                   sizeof(prefix_meta->block_hash)) == 0 &&
           prefix_meta->hot_segment_id == block_meta->hot_segment_id &&
           prefix_meta->placement_node == block_meta->placement_node &&
           prefix_meta->placement_level == block_meta->placement_level &&
           prefix_meta->state == block_meta->state;
}

bool mem_service_group_covers_blocks(const struct mem_service_record *group_meta,
                               const struct mem_service_record *primary_block_meta,
                               const struct mem_service_record *aux_block_meta)
{
    if (!group_meta || !primary_block_meta || !aux_block_meta) {
        return false;
    }
    if (group_meta->kind != MEM_SERVICE_RECORD_PREFIX_GROUP ||
        primary_block_meta->kind != MEM_SERVICE_RECORD_BLOCK_META ||
        aux_block_meta->kind != MEM_SERVICE_RECORD_BLOCK_META) {
        return false;
    }
    return group_meta->member_count >= 2 &&
           group_meta->last_result_segment != 0 &&
           mem_service_record_has_member(group_meta, primary_block_meta->block_hash) &&
           mem_service_record_has_member(group_meta, aux_block_meta->block_hash);
}

const char *mem_service_kvcache_state_name(enum mem_service_kvcache_state state)
{
    switch (state) {
    case MEM_SERVICE_KVCACHE_STATE_MISSING:
        return "missing";
    case MEM_SERVICE_KVCACHE_STATE_FILLED:
        return "filled";
    case MEM_SERVICE_KVCACHE_STATE_HOT:
        return "hot";
    case MEM_SERVICE_KVCACHE_STATE_RELOADED:
        return "reloaded";
    default:
        return "unknown";
    }
}

int mem_service_init(struct mem_service *svc,
                       bool shmem_ready,
                       bool urma_ready,
                       bool block_ready)
{
    if (!svc) {
        return -1;
    }
    memset(svc, 0, sizeof(*svc));
    svc->shmem_ready = shmem_ready;
    svc->urma_ready = urma_ready;
    svc->block_ready = block_ready;
    if (!svc->shmem_ready || !svc->urma_ready || !svc->block_ready) {
        return -1;
    }
    return 0;
}

int mem_service_get_record(struct mem_service *svc, const char *key, struct mem_service_record *out)
{
    struct mem_service_record *rec = mem_service_find_record(svc, key);

    if (!rec || !out) {
        return -1;
    }
    memcpy(out, rec, sizeof(*out));
    return 0;
}

int mem_service_bootstrap_kvcache(struct mem_service *svc,
                            const struct mem_service_block_ctx *ctx,
                            struct mem_service_record *resolved_out)
{
    char prefix_key[96];
    char group_key[96];
    char block_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (mem_service_build_prefix_key(ctx, prefix_key, sizeof(prefix_key)) != 0 ||
        mem_service_build_group_key(ctx, group_key, sizeof(group_key)) != 0 ||
        mem_service_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }

    if (mem_service_put_prefix_group(svc,
                               group_key,
                               ctx->request_id,
                               ctx->group_id,
                               ctx->block_hash,
                               ctx->placement_node,
                               ctx->placement_level,
                               ctx->hot_segment_id,
                               MEM_SERVICE_KVCACHE_STATE_FILLED,
                               0) != 0) {
        return -1;
    }
    if (mem_service_put_request_prefix(svc,
                                 prefix_key,
                                 ctx->request_id,
                                 ctx->prefix_group,
                                 ctx->group_id,
                                 ctx->block_hash) != 0) {
        return -1;
    }
    printf("[mem_service] stage db_service_bootstrap=request_prefix_ok key=%s request=%s prefix=%s block=%s\n",
           prefix_key,
           ctx->request_id,
           ctx->prefix_group,
           ctx->block_hash);

    if (mem_service_put_block_meta(svc,
                             block_key,
                             ctx->request_id,
                             ctx->prefix_group,
                             ctx->group_id,
                             ctx->block_hash,
                             ctx->placement_node,
                             ctx->placement_level,
                             ctx->hot_segment_id,
                             MEM_SERVICE_KVCACHE_STATE_FILLED) != 0) {
        return -1;
    }
    printf("[mem_service] stage db_service_bootstrap=block_meta_ok key=%s placement_node=%u placement_level=%u hot_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->placement_node,
           ctx->placement_level,
           ctx->hot_segment_id,
           mem_service_kvcache_state_name(MEM_SERVICE_KVCACHE_STATE_FILLED));

    if (mem_service_update_block_result(svc,
                                  block_key,
                                  ctx->result_segment_id,
                                  MEM_SERVICE_KVCACHE_STATE_HOT) != 0) {
        return -1;
    }
    if (mem_service_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (mem_service_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    printf("[mem_service] stage db_service_bootstrap=result_update_ok key=%s result_segment=0x%016" PRIx64 " state=%s\n",
           block_key,
           ctx->result_segment_id,
           mem_service_kvcache_state_name(MEM_SERVICE_KVCACHE_STATE_HOT));

    if (mem_service_update_prefix_result(svc,
                                   prefix_key,
                                   ctx,
                                   resolved_out) != 0) {
        return -1;
    }
    printf("[mem_service] stage db_service_bootstrap=prefix_result_ok key=%s block=%s hot_segment=0x%016" PRIx64 " state=%s result_segment=0x%016" PRIx64 " version=%" PRIu64 "\n",
           prefix_key,
           resolved_out->block_hash,
           resolved_out->hot_segment_id,
           mem_service_kvcache_state_name(resolved_out->state),
           resolved_out->last_result_segment,
           resolved_out->version);

    if (mem_service_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int mem_service_update_prefix_metadata(struct mem_service *svc,
                                 const struct mem_service_block_ctx *ctx,
                                 const struct mem_service_record *block_record,
                                 struct mem_service_record *resolved_out)
{
    char prefix_key[96];
    int rc;

    if (!svc || !ctx || !block_record || !resolved_out) {
        return -1;
    }

    if (mem_service_build_prefix_key(ctx, prefix_key, sizeof(prefix_key)) != 0) {
        return -1;
    }
    rc = mem_service_update_prefix_result(svc, prefix_key, ctx, block_record);
    if (rc != 0) {
        return rc;
    }
    if (mem_service_update_prefix_group_from_block(svc, ctx, block_record) != 0) {
        return -1;
    }
    if (mem_service_get_record(svc, prefix_key, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int mem_service_get_prefix_group_metadata(struct mem_service *svc,
                                    const struct mem_service_block_ctx *ctx,
                                    struct mem_service_record *resolved_out)
{
    char group_key[96];

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }
    if (mem_service_build_group_key(ctx, group_key, sizeof(group_key)) != 0) {
        return -1;
    }
    return mem_service_get_record(svc, group_key, resolved_out);
}

int mem_service_apply_block_result(struct mem_service *svc,
                             const struct mem_service_block_ctx *ctx,
                             uint64_t result_segment_id,
                             enum mem_service_kvcache_state next_state,
                             struct mem_service_record *resolved_out)
{
    char block_key[96];
    struct mem_service_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (mem_service_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (mem_service_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = mem_service_update_block_result(svc, block_key, result_segment_id, next_state);
    if (rc != 0) {
        return rc;
    }
    if (mem_service_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (mem_service_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int mem_service_rebind_block_view(struct mem_service *svc,
                            const struct mem_service_block_ctx *ctx,
                            uint64_t hot_segment_id,
                            uint32_t placement_level,
                            struct mem_service_record *resolved_out)
{
    char block_key[96];
    struct mem_service_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (mem_service_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (mem_service_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = mem_service_update_block_view(svc, block_key, hot_segment_id, placement_level);
    if (rc != 0) {
        return rc;
    }
    if (mem_service_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (mem_service_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}

int mem_service_handoff_block_owner(struct mem_service *svc,
                              const struct mem_service_block_ctx *ctx,
                              uint32_t placement_node,
                              uint32_t placement_level,
                              uint64_t hot_segment_id,
                              struct mem_service_record *resolved_out)
{
    char block_key[96];
    struct mem_service_record current;
    int rc;

    if (!svc || !ctx || !resolved_out) {
        return -1;
    }

    if (mem_service_build_block_key(ctx, block_key, sizeof(block_key)) != 0) {
        return -1;
    }
    if (mem_service_get_record(svc, block_key, &current) != 0) {
        return -1;
    }
    if (current.placement_node != ctx->placement_node) {
        return 2;
    }
    rc = mem_service_update_block_owner(svc,
                                  block_key,
                                  placement_node,
                                  placement_level,
                                  hot_segment_id);
    if (rc != 0) {
        return rc;
    }
    if (mem_service_get_record(svc, block_key, resolved_out) != 0) {
        return -1;
    }
    if (mem_service_update_prefix_group_from_block(svc, ctx, resolved_out) != 0) {
        return -1;
    }
    return 0;
}
