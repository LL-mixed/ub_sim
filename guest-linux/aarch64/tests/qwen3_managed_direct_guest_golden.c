#include "qwen3_managed_direct_guest.h"

#include <errno.h>
#include <math.h>
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

static struct qwen3_pto_guest_geometry expected;
static unsigned prepares, submits, polls, releases;

int lingqu_model_direct_prepare(
    const struct lingqu_model_direct_request *request,
    struct lingqu_model_direct_operation **operation_out)
{
    uint64_t kv_width = (uint64_t)expected.kv_heads * expected.head_dim;
    uint64_t previous_words = (expected.end - expected.first) *
        (10 + 2 * (uint64_t)expected.past * kv_width);
    uint64_t next_words = (expected.end - expected.first) *
        (10 + 2 * ((uint64_t)expected.past + expected.tokens) * kv_width);
    uint32_t shapes[6][2] = {
        {expected.first ? expected.tokens : 1,
         expected.first ? expected.hidden : 1},
        {1, expected.past ? (uint32_t)previous_words : 1},
        {1, expected.first ? 1 : expected.tokens},
        {expected.tokens, expected.hidden},
        {1, (uint32_t)next_words},
        {1, expected.end == expected.layers ? expected.vocab : 1},
    };
    uint32_t scalar_values[12] = {
        expected.first, expected.end, expected.layers, expected.past,
        expected.tokens, expected.hidden, expected.intermediate,
        expected.query_heads, expected.kv_heads, expected.head_dim,
        expected.vocab, expected.scale_bits,
    };

    CHECK(request && operation_out && request->compute == (void *)1);
    CHECK(request->endpoint == (void *)2 && request->op_id == 101);
    CHECK(request->request_id == 202 && request->callable_id == 3);
    CHECK(request->artifact_fingerprint == 303 && request->requester_cna == 7);
    CHECK(request->memref_count == 6 && request->scalar_count == 12);
    CHECK(request->metadata == (void *)3 && request->metadata_bytes == 4096);
    CHECK(request->metadata_iova == 0x1000);
    for (uint32_t i = 0; i < 6; ++i) {
        const struct lingqu_model_direct_memref *arg = &request->memrefs[i];
        uint16_t dtype = (i == 3 || (i == 0 && expected.first)) ? 1 : 0;
        uint8_t access = i == 4 ? LINGQU_SHMEM_ACCESS_READ_WRITE :
            (i == 3 || (i == 5 && expected.end == expected.layers) ?
             LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ);
        CHECK(arg->owner == (void *)(uintptr_t)(10 + i));
        CHECK(arg->arg_index == i && arg->spec.byte_offset == i * 4096);
        CHECK(arg->spec.rank == 2 && arg->spec.dtype == dtype);
        CHECK(arg->spec.access == access);
        CHECK(arg->spec.shape[0] == shapes[i][0]);
        CHECK(arg->spec.shape[1] == shapes[i][1]);
        CHECK(arg->spec.strides[0] == shapes[i][1]);
        CHECK(arg->spec.strides[1] == 1);
        CHECK(arg->spec.byte_length ==
              (uint64_t)shapes[i][0] * shapes[i][1] * (dtype ? 2 : 4));
    }
    for (uint32_t i = 0; i < 12; ++i) {
        CHECK(request->scalars[i].arg_index == 6 + i);
        CHECK(request->scalars[i].dtype == 8);
        CHECK(request->scalars[i].value == scalar_values[i]);
    }
    *operation_out = calloc(1, sizeof(**operation_out));
    CHECK(*operation_out);
    (*operation_out)->marker = 0x5133;
    ++prepares;
    return 0;
}

int lingqu_model_direct_submit(struct lingqu_model_direct_operation *operation)
{
    CHECK(operation && operation->marker == 0x5133);
    ++submits;
    return 0;
}

int lingqu_model_direct_poll(
    struct lingqu_model_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion)
{
    CHECK(operation && operation->marker == 0x5133 && completion);
    *completion = (struct lingqu_shmem_pto_completion) {
        .op_id = 101, .status = 1,
    };
    ++polls;
    return 0;
}

int lingqu_model_direct_release(
    struct lingqu_model_direct_operation **operation)
{
    CHECK(operation && *operation && (*operation)->marker == 0x5133);
    free(*operation);
    *operation = NULL;
    ++releases;
    return 0;
}

static void run_case(uint32_t first, uint32_t end, uint32_t past,
                     uint32_t tokens)
{
    struct qwen3_managed_direct_request request = {0};
    struct qwen3_managed_direct_operation *operation = NULL;
    struct lingqu_shmem_pto_completion completion;
    float scale = 1.0f / sqrtf(8.0f);

    request.geometry = (struct qwen3_pto_guest_geometry) {
        .first = first, .end = end, .layers = 12, .past = past,
        .tokens = tokens, .hidden = 16, .intermediate = 32,
        .query_heads = 4, .kv_heads = 2, .head_dim = 8, .vocab = 128,
    };
    memcpy(&request.geometry.scale_bits, &scale, sizeof(scale));
    expected = request.geometry;
    request.compute = (void *)1;
    request.endpoint = (void *)2;
    request.op_id = 101;
    request.request_id = 202;
    request.artifact_fingerprint = 303;
    request.requester_cna = 7;
    request.metadata = (void *)3;
    request.metadata_bytes = 4096;
    request.metadata_iova = 0x1000;
    for (uint32_t i = 0; i < 6; ++i) {
        request.owners[i] = (void *)(uintptr_t)(10 + i);
        request.offsets[i] = i * 4096;
    }
    CHECK(!qwen3_managed_direct_prepare(&request, &operation));
    CHECK(operation);
    CHECK(!qwen3_managed_direct_submit(operation));
    CHECK(!qwen3_managed_direct_poll(operation, &completion));
    CHECK(completion.status == 1);
    CHECK(!qwen3_managed_direct_release(&operation));
    CHECK(!operation);
}

int main(void)
{
    struct qwen3_managed_direct_request invalid = {0};
    struct qwen3_managed_direct_operation *operation = NULL;

    run_case(0, 4, 0, 2);
    run_case(4, 8, 2, 1);
    run_case(8, 12, 3, 1);
    CHECK(qwen3_managed_direct_prepare(&invalid, &operation) == -EINVAL);
    CHECK(!operation);
    CHECK(prepares == 3 && submits == 3 && polls == 3 && releases == 3);
    puts("qwen3_managed_direct_guest=pass cases=4 callable=3 staging_bytes=0");
    return 0;
}
