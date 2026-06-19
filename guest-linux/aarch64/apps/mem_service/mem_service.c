#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "components/llm_infer/llm_infer.h"
#include "components/mem_service/mem_service.h"

static void usage(const char *argv0)
{
    printf("Usage: %s [--smoke] [--inspect-qwen3]\n", argv0);
}

static int run_smoke(void)
{
    struct mem_service svc;
    struct mem_service_block_ctx ctx;
    struct mem_service_block_ctx aux_ctx;
    struct mem_service_record block;
    struct mem_service_record aux_block;
    struct mem_service_record prefix;
    struct mem_service_record aux_prefix;
    struct mem_service_record group;
    char block_key[96];

    memset(&svc, 0, sizeof(svc));
    memset(&ctx, 0, sizeof(ctx));
    memset(&aux_ctx, 0, sizeof(aux_ctx));
    memset(&block, 0, sizeof(block));
    memset(&aux_block, 0, sizeof(aux_block));
    memset(&prefix, 0, sizeof(prefix));
    memset(&aux_prefix, 0, sizeof(aux_prefix));
    memset(&group, 0, sizeof(group));

    snprintf(ctx.request_id, sizeof(ctx.request_id), "cli-smoke-request");
    snprintf(ctx.prefix_group, sizeof(ctx.prefix_group), "cli-prefix");
    snprintf(ctx.group_id, sizeof(ctx.group_id), "cli-group");
    snprintf(ctx.block_hash, sizeof(ctx.block_hash), "cli-block-hash");
    ctx.placement_node = 1;
    ctx.placement_level = 2;
    ctx.hot_segment_id = 0x1000;
    ctx.result_segment_id = 0x2000;
    aux_ctx = ctx;
    snprintf(aux_ctx.prefix_group, sizeof(aux_ctx.prefix_group), "cli-prefix-aux");
    snprintf(aux_ctx.block_hash, sizeof(aux_ctx.block_hash), "cli-block-hash-aux");
    aux_ctx.hot_segment_id = 0x3000;
    aux_ctx.result_segment_id = 0x4000;

    if (mem_service_init(&svc, true, true, true) != 0) {
        fprintf(stderr, "mem_service smoke: init failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &ctx, &block) != 0) {
        fprintf(stderr, "mem_service smoke: bootstrap failed\n");
        return 1;
    }
    if (mem_service_bootstrap_kvcache(&svc, &aux_ctx, &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: aux bootstrap failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &ctx,
                                       ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &block) != 0) {
        fprintf(stderr, "mem_service smoke: apply block result failed\n");
        return 1;
    }
    if (mem_service_apply_block_result(&svc,
                                       &aux_ctx,
                                       aux_ctx.result_segment_id + 0x10,
                                       MEM_SERVICE_KVCACHE_STATE_RELOADED,
                                       &aux_block) != 0) {
        fprintf(stderr, "mem_service smoke: apply aux block result failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &ctx, &block, &prefix) != 0) {
        fprintf(stderr, "mem_service smoke: prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_update_prefix_metadata(&svc, &aux_ctx, &aux_block, &aux_prefix) != 0) {
        fprintf(stderr, "mem_service smoke: aux prefix metadata update failed\n");
        return 1;
    }
    if (mem_service_get_prefix_group_metadata(&svc, &ctx, &group) != 0) {
        fprintf(stderr, "mem_service smoke: prefix group metadata failed\n");
        return 1;
    }
    mem_service_build_block_key_from_hash(ctx.block_hash, block_key, sizeof(block_key));
    if (mem_service_get_record(&svc, block_key, &block) != 0) {
        fprintf(stderr, "mem_service smoke: block lookup failed\n");
        return 1;
    }
    if (!mem_service_prefix_matches_block_meta(&prefix, &block) ||
        !mem_service_prefix_matches_block_meta(&aux_prefix, &aux_block) ||
        !mem_service_group_covers_blocks(&group, &block, &aux_block)) {
        fprintf(stderr, "mem_service smoke: prefix/group relation failed\n");
        return 1;
    }

    printf("mem_service smoke: status=ok records=%zu block_key=%s state=%s group_members=%u\n",
           svc.record_count,
           block_key,
           mem_service_kvcache_state_name(block.state),
           group.member_count);
    return 0;
}

static int inspect_qwen3(void)
{
    uint32_t node;
    uint32_t nodes = (uint32_t)llm_infer_qwen3_pipeline_nodes();

    printf("mem_service qwen3: model_key=%s nodes=%u layers=%" PRIu64
           " hidden_range_bytes=%" PRIu64 " decode_hidden_bytes=%" PRIu64 "\n",
           llm_infer_qwen3_model_key(),
           nodes,
           llm_infer_qwen3_total_layers(),
           llm_infer_qwen3_hidden_range_bytes(),
           llm_infer_qwen3_decode_hidden_bytes());
    for (node = 0; node < nodes; ++node) {
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t next = 0;

        if (llm_infer_qwen3_layer_range_for_node(node, nodes, &start, &end, &next) != 0) {
            fprintf(stderr, "mem_service qwen3: invalid placement node=%u\n", node);
            return 1;
        }
        printf("mem_service qwen3: node=%u layers=[%u,%u) next=%u kv_bytes_per_token=%" PRIu64 "\n",
               node + 1,
               start,
               end,
               next + 1,
               llm_infer_qwen3_range_kv_state_bytes(start, end));
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 1 || strcmp(argv[1], "--smoke") == 0) {
        return run_smoke();
    }
    if (strcmp(argv[1], "--inspect-qwen3") == 0) {
        return inspect_qwen3();
    }
    usage(argv[0]);
    return 2;
}
