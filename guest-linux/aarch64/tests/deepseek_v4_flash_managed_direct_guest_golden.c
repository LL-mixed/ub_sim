#include "deepseek_v4_flash_managed_direct_guest.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { \
        fprintf(stderr, "line %d: %s\n", __LINE__, #value); \
        exit(1); \
    } \
} while (0)

struct lingqu_model_direct_operation {
    unsigned marker;
};

static struct deepseek_v4_flash_managed_direct_description description;
static unsigned prepares, submits, polls, releases;

int lingqu_model_direct_prepare(
    const struct lingqu_model_direct_request *request,
    struct lingqu_model_direct_operation **operation_out)
{
    CHECK(request && operation_out && request->compute == (void *)1);
    CHECK(request->endpoint == (void *)2 && request->op_id == 501);
    CHECK(request->request_id == 502 && request->callable_id == 5);
    CHECK(request->artifact_fingerprint == 503 && request->requester_cna == 7);
    CHECK(request->memref_count == 6 && request->scalar_count == 23);
    CHECK(request->metadata == (void *)3 && request->metadata_bytes == 4096);
    CHECK(request->metadata_iova == 0x5000);
    for (uint32_t i = 0; i < 6; ++i) {
        const struct lingqu_model_direct_memref *arg = &request->memrefs[i];
        CHECK(arg->owner == (void *)(uintptr_t)(20 + i));
        CHECK(arg->arg_index == i && arg->spec.byte_offset == i * 8192);
        CHECK(!memcmp(&arg->spec, &description.tensors[i],
                      sizeof(arg->spec)));
        CHECK(arg->spec.byte_offset == i * 8192);
    }
    for (uint32_t i = 0; i < 23; ++i) {
        CHECK(request->scalars[i].arg_index == 6 + i);
        CHECK(request->scalars[i].dtype == 8);
        CHECK(request->scalars[i].value == description.scalars[i]);
    }
    *operation_out = calloc(1, sizeof(**operation_out));
    CHECK(*operation_out);
    (*operation_out)->marker = 0xd545;
    ++prepares;
    return 0;
}

int lingqu_model_direct_submit(struct lingqu_model_direct_operation *operation)
{
    CHECK(operation && operation->marker == 0xd545);
    ++submits;
    return 0;
}

int lingqu_model_direct_poll(
    struct lingqu_model_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion)
{
    CHECK(operation && operation->marker == 0xd545 && completion);
    *completion = (struct lingqu_shmem_pto_completion) {
        .op_id = 501, .status = 1,
    };
    ++polls;
    return 0;
}

int lingqu_model_direct_release(
    struct lingqu_model_direct_operation **operation)
{
    CHECK(operation && *operation && (*operation)->marker == 0xd545);
    free(*operation);
    *operation = NULL;
    ++releases;
    return 0;
}

static struct deepseek_v4_flash_managed_direct_geometry geometry(
    uint32_t first, uint32_t end, uint32_t past, uint32_t tokens)
{
    return (struct deepseek_v4_flash_managed_direct_geometry) {
        .first = first,
        .end = end,
        .layers = 43,
        .past = past,
        .tokens = tokens,
        .hidden = 4096,
        .hc_mult = 4,
        .vocab = 129280,
        .attention_heads = 64,
        .kv_heads = 1,
        .head_dim = 512,
        .rope_head_dim = 64,
        .q_lora_rank = 1024,
        .o_lora_rank = 1024,
        .output_groups = 8,
        .experts = 256,
        .experts_used = 6,
        .shared_experts = 1,
        .moe_intermediate = 2048,
        .sliding_window = 128,
        .indexer_heads = 64,
        .indexer_head_dim = 128,
        .indexer_top_k = 512,
    };
}

static void run_case(uint32_t first, uint32_t end, uint32_t past,
                     uint32_t tokens, uint64_t previous_bytes,
                     uint64_t next_bytes)
{
    struct deepseek_v4_flash_managed_direct_request request = {0};
    struct deepseek_v4_flash_managed_direct_operation *operation = NULL;
    struct lingqu_shmem_pto_completion completion;

    request.geometry = geometry(first, end, past, tokens);
    CHECK(!deepseek_v4_flash_managed_direct_describe(&request.geometry,
                                                      &description));
    CHECK(description.tensors[0].byte_length ==
          (first ? (uint64_t)tokens * 4 * 4096 * sizeof(float) :
                   sizeof(float)));
    CHECK(description.tensors[1].byte_length == previous_bytes);
    CHECK(description.tensors[2].byte_length ==
          (uint64_t)tokens * sizeof(uint32_t));
    CHECK(description.tensors[3].byte_length ==
          (uint64_t)tokens * 4 * 4096 * sizeof(float));
    CHECK(description.tensors[4].byte_length == next_bytes);
    CHECK(description.tensors[5].byte_length ==
          (end == 43 ? UINT64_C(129280) * sizeof(float) : sizeof(float)));
    request.compute = (void *)1;
    request.endpoint = (void *)2;
    request.op_id = 501;
    request.request_id = 502;
    request.artifact_fingerprint = 503;
    request.requester_cna = 7;
    request.metadata = (void *)3;
    request.metadata_bytes = 4096;
    request.metadata_iova = 0x5000;
    for (uint32_t i = 0; i < 6; ++i) {
        request.owners[i] = (void *)(uintptr_t)(20 + i);
        request.offsets[i] = i * 8192;
        description.tensors[i].byte_offset = request.offsets[i];
    }
    CHECK(!deepseek_v4_flash_managed_direct_prepare(&request, &operation));
    CHECK(operation);
    CHECK(!deepseek_v4_flash_managed_direct_submit(operation));
    CHECK(!deepseek_v4_flash_managed_direct_poll(operation, &completion));
    CHECK(completion.status == 1);
    CHECK(!deepseek_v4_flash_managed_direct_release(&operation));
    CHECK(!operation);
}

int main(void)
{
    struct deepseek_v4_flash_managed_direct_geometry invalid = {0};
    struct deepseek_v4_flash_managed_direct_description invalid_description;

    run_case(0, 2, 0, 3, 1, 12352);
    run_case(2, 4, 3, 1, 618680, 625336);
    run_case(41, 43, 4, 1, 625336, 629432);
    CHECK(deepseek_v4_flash_managed_direct_describe(
              &invalid, &invalid_description) == -EINVAL);
    CHECK(prepares == 3 && submits == 3 && polls == 3 && releases == 3);
    puts("deepseek_v4_flash_managed_direct_guest=pass cases=4 "
         "callable=5 staging_bytes=0");
    return 0;
}
