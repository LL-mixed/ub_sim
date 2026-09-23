/* SPDX-License-Identifier: MIT */
#include "deepseek_v4_flash_managed_direct_guest.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define DS4_LAYERS 43u
#define DS4_HIDDEN 4096u
#define DS4_HC_MULT 4u
#define DS4_VOCAB 129280u
#define DS4_ATTENTION_HEADS 64u
#define DS4_KV_HEADS 1u
#define DS4_HEAD_DIM 512u
#define DS4_ROPE_HEAD_DIM 64u
#define DS4_Q_LORA_RANK 1024u
#define DS4_O_LORA_RANK 1024u
#define DS4_OUTPUT_GROUPS 8u
#define DS4_EXPERTS 256u
#define DS4_EXPERTS_USED 6u
#define DS4_SHARED_EXPERTS 1u
#define DS4_MOE_INTERMEDIATE 2048u
#define DS4_SLIDING_WINDOW 128u
#define DS4_INDEXER_HEADS 64u
#define DS4_INDEXER_HEAD_DIM 128u
#define DS4_INDEXER_TOP_K 512u
#define DS4_MAX_TOKENS (1u << 20)

#define DS4_DTYPE_F32 0u
#define DS4_DTYPE_I32 2u
#define DS4_DTYPE_U8 5u

static const uint32_t ds4_compress_ratios[DS4_LAYERS] = {
    0, 0, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4,
    128, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128,
    4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4, 128, 4,
};

struct deepseek_v4_flash_managed_direct_operation {
    struct lingqu_model_direct_operation *direct;
};

_Static_assert(sizeof(struct deepseek_v4_flash_managed_direct_geometry) ==
               DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT * sizeof(uint32_t),
               "DeepSeek direct geometry must remain a packed u32 scalar ABI");

static int add_u64(uint64_t *value, uint64_t addend)
{
    if (!value || *value > UINT64_MAX - addend) return -EOVERFLOW;
    *value += addend;
    return 0;
}

static int mul_u64(uint64_t left, uint64_t right, uint64_t *value_out)
{
    if (!value_out || (right && left > UINT64_MAX / right))
        return -EOVERFLOW;
    *value_out = left * right;
    return 0;
}

static int f32_vector_bytes(uint64_t elements, uint64_t *bytes_out)
{
    uint64_t bytes;
    int rc = mul_u64(elements, sizeof(float), &bytes);
    if (!rc) rc = add_u64(&bytes, sizeof(uint64_t));
    if (!rc) *bytes_out = bytes;
    return rc;
}

static int compressor_bytes(uint32_t head_dim, uint32_t ratio,
                            uint64_t *bytes_out)
{
    uint64_t lanes = ratio == 4u ? 2u : 1u;
    uint64_t width;
    uint64_t rows;
    uint64_t elements;
    uint64_t bytes = 4u * sizeof(uint64_t);
    int rc;

    if (!bytes_out || (ratio != 4u && ratio != 128u)) return -EINVAL;
    rc = mul_u64(lanes, head_dim, &width);
    if (!rc) rc = mul_u64(ratio, lanes, &rows);
    if (!rc) rc = mul_u64(rows, width, &elements);
    if (!rc) rc = mul_u64(elements, 2u * sizeof(float), &elements);
    if (!rc) rc = add_u64(&bytes, elements);
    if (!rc) *bytes_out = bytes;
    return rc;
}

static int range_state_bytes(
    const struct deepseek_v4_flash_managed_direct_geometry *geometry,
    uint32_t positions, uint64_t *bytes_out)
{
    uint64_t raw_rows = positions < geometry->sliding_window ?
        positions : geometry->sliding_window;
    uint64_t bytes = 4u * sizeof(uint64_t);
    uint64_t raw;
    int rc;

    if (!geometry || !bytes_out || !positions) return -EINVAL;
    rc = mul_u64(raw_rows, geometry->head_dim, &raw);
    for (uint32_t layer = geometry->first; !rc && layer < geometry->end;
         ++layer) {
        uint32_t ratio = ds4_compress_ratios[layer];
        uint64_t vector_bytes;

        rc = add_u64(&bytes, sizeof(uint64_t));
        if (!rc) rc = f32_vector_bytes(raw, &vector_bytes);
        if (!rc) rc = add_u64(&bytes, vector_bytes);
        if (rc || !ratio) continue;
        {
            uint64_t compressed_rows = positions / ratio;
            uint64_t compressed;
            uint64_t snapshot;

            if (compressed_rows > geometry->indexer_top_k)
                compressed_rows = geometry->indexer_top_k;
            rc = mul_u64(compressed_rows, geometry->head_dim, &compressed);
            if (!rc) rc = f32_vector_bytes(compressed, &vector_bytes);
            if (!rc) rc = add_u64(&bytes, vector_bytes);
            if (!rc && ratio == 4u) {
                rc = mul_u64(compressed_rows, geometry->indexer_head_dim,
                             &compressed);
                if (!rc) rc = f32_vector_bytes(compressed, &vector_bytes);
                if (!rc) rc = add_u64(&bytes, vector_bytes);
            }
            if (!rc) rc = compressor_bytes(geometry->head_dim, ratio,
                                            &snapshot);
            if (!rc) rc = add_u64(&bytes, snapshot);
            if (!rc && ratio == 4u) {
                rc = compressor_bytes(geometry->indexer_head_dim, ratio,
                                      &snapshot);
                if (!rc) rc = add_u64(&bytes, snapshot);
            }
        }
    }
    if (!rc) *bytes_out = bytes;
    return rc;
}

static int validate_geometry(
    const struct deepseek_v4_flash_managed_direct_geometry *geometry)
{
    uint64_t total;

    if (!geometry || geometry->first >= geometry->end ||
        geometry->end > geometry->layers ||
        geometry->layers != DS4_LAYERS || !geometry->tokens ||
        geometry->hidden != DS4_HIDDEN || geometry->hc_mult != DS4_HC_MULT ||
        geometry->vocab != DS4_VOCAB ||
        geometry->attention_heads != DS4_ATTENTION_HEADS ||
        geometry->kv_heads != DS4_KV_HEADS ||
        geometry->head_dim != DS4_HEAD_DIM ||
        geometry->rope_head_dim != DS4_ROPE_HEAD_DIM ||
        geometry->q_lora_rank != DS4_Q_LORA_RANK ||
        geometry->o_lora_rank != DS4_O_LORA_RANK ||
        geometry->output_groups != DS4_OUTPUT_GROUPS ||
        geometry->experts != DS4_EXPERTS ||
        geometry->experts_used != DS4_EXPERTS_USED ||
        geometry->shared_experts != DS4_SHARED_EXPERTS ||
        geometry->moe_intermediate != DS4_MOE_INTERMEDIATE ||
        geometry->sliding_window != DS4_SLIDING_WINDOW ||
        geometry->indexer_heads != DS4_INDEXER_HEADS ||
        geometry->indexer_head_dim != DS4_INDEXER_HEAD_DIM ||
        geometry->indexer_top_k != DS4_INDEXER_TOP_K)
        return -EINVAL;
    total = (uint64_t)geometry->past + geometry->tokens;
    return total > DS4_MAX_TOKENS ? -ERANGE : 0;
}

int deepseek_v4_flash_managed_direct_geometry_init(
    uint32_t first,
    uint32_t end,
    uint32_t past,
    uint32_t tokens,
    struct deepseek_v4_flash_managed_direct_geometry *geometry_out)
{
    if (!geometry_out)
        return -EINVAL;
    *geometry_out = (struct deepseek_v4_flash_managed_direct_geometry) {
        .first = first,
        .end = end,
        .layers = DS4_LAYERS,
        .past = past,
        .tokens = tokens,
        .hidden = DS4_HIDDEN,
        .hc_mult = DS4_HC_MULT,
        .vocab = DS4_VOCAB,
        .attention_heads = DS4_ATTENTION_HEADS,
        .kv_heads = DS4_KV_HEADS,
        .head_dim = DS4_HEAD_DIM,
        .rope_head_dim = DS4_ROPE_HEAD_DIM,
        .q_lora_rank = DS4_Q_LORA_RANK,
        .o_lora_rank = DS4_O_LORA_RANK,
        .output_groups = DS4_OUTPUT_GROUPS,
        .experts = DS4_EXPERTS,
        .experts_used = DS4_EXPERTS_USED,
        .shared_experts = DS4_SHARED_EXPERTS,
        .moe_intermediate = DS4_MOE_INTERMEDIATE,
        .sliding_window = DS4_SLIDING_WINDOW,
        .indexer_heads = DS4_INDEXER_HEADS,
        .indexer_head_dim = DS4_INDEXER_HEAD_DIM,
        .indexer_top_k = DS4_INDEXER_TOP_K,
    };
    return validate_geometry(geometry_out);
}

static void set_1d(struct lingqu_shmem_memref_spec *spec, uint64_t elements,
                   uint16_t dtype, uint8_t access, uint64_t width)
{
    *spec = (struct lingqu_shmem_memref_spec) {
        .byte_length = elements * width,
        .rank = 1,
        .dtype = dtype,
        .access = access,
        .shape = {(uint32_t)elements},
        .strides = {1},
    };
}

static void set_2d(struct lingqu_shmem_memref_spec *spec, uint32_t rows,
                   uint32_t columns, uint16_t dtype, uint8_t access,
                   uint64_t width)
{
    *spec = (struct lingqu_shmem_memref_spec) {
        .byte_length = (uint64_t)rows * columns * width,
        .rank = 2,
        .dtype = dtype,
        .access = access,
        .shape = {rows, columns},
        .strides = {columns, 1},
    };
}

int deepseek_v4_flash_managed_direct_describe(
    const struct deepseek_v4_flash_managed_direct_geometry *geometry,
    struct deepseek_v4_flash_managed_direct_description *description_out)
{
    uint64_t previous_bytes = 1;
    uint64_t next_bytes;
    uint64_t hidden_width;
    uint64_t total;
    int rc;

    if (!description_out) return -EINVAL;
    memset(description_out, 0, sizeof(*description_out));
    rc = validate_geometry(geometry);
    if (rc) return rc;
    total = (uint64_t)geometry->past + geometry->tokens;
    hidden_width = (uint64_t)geometry->hidden * geometry->hc_mult;
    if (hidden_width > UINT32_MAX) return -EOVERFLOW;
    if (geometry->past)
        rc = range_state_bytes(geometry, geometry->past, &previous_bytes);
    if (!rc) rc = range_state_bytes(geometry, (uint32_t)total, &next_bytes);
    if (rc || previous_bytes > UINT32_MAX || next_bytes > UINT32_MAX)
        return rc ? rc : -EOVERFLOW;

    set_2d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_HIDDEN_INPUT],
        geometry->first ? geometry->tokens : 1,
        geometry->first ? (uint32_t)hidden_width : 1,
        DS4_DTYPE_F32, LINGQU_SHMEM_ACCESS_READ, sizeof(float));
    set_1d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_PREVIOUS_KV],
        previous_bytes, DS4_DTYPE_U8, LINGQU_SHMEM_ACCESS_READ, 1);
    set_1d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TOKEN_IDS],
        geometry->tokens, DS4_DTYPE_I32,
        LINGQU_SHMEM_ACCESS_READ, sizeof(uint32_t));
    set_2d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_HIDDEN_OUTPUT],
        geometry->tokens, (uint32_t)hidden_width, DS4_DTYPE_F32,
        LINGQU_SHMEM_ACCESS_WRITE, sizeof(float));
    set_1d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_NEXT_KV],
        next_bytes, DS4_DTYPE_U8, LINGQU_SHMEM_ACCESS_READ_WRITE, 1);
    set_1d(&description_out
               ->tensors[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_LOGITS],
        geometry->end == geometry->layers ? geometry->vocab : 1,
        DS4_DTYPE_F32,
        geometry->end == geometry->layers ? LINGQU_SHMEM_ACCESS_WRITE :
            LINGQU_SHMEM_ACCESS_READ,
        sizeof(float));
    memcpy(description_out->scalars, geometry,
           sizeof(description_out->scalars));
    return 0;
}

int deepseek_v4_flash_managed_direct_prepare(
    const struct deepseek_v4_flash_managed_direct_request *request,
    struct deepseek_v4_flash_managed_direct_operation **operation_out)
{
    struct deepseek_v4_flash_managed_direct_operation *operation;
    struct lingqu_model_direct_memref
        memrefs[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT];
    struct lingqu_shmem_pto_scalar_desc
        scalars[DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT];
    struct lingqu_model_direct_request direct;
    struct deepseek_v4_flash_managed_direct_description description;
    int rc;

    if (!operation_out) return -EINVAL;
    *operation_out = NULL;
    if (!request || !request->compute || !request->endpoint ||
        !request->op_id || !request->request_id ||
        !request->artifact_fingerprint || !request->requester_cna ||
        !request->metadata || !request->metadata_bytes ||
        !request->metadata_iova) return -EINVAL;
    rc = deepseek_v4_flash_managed_direct_describe(&request->geometry,
                                                    &description);
    if (rc) return rc;
    for (uint32_t i = 0;
         i < DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT; ++i) {
        if (!request->owners[i]) return -EINVAL;
        memrefs[i] = (struct lingqu_model_direct_memref) {
            .owner = request->owners[i],
            .spec = description.tensors[i],
            .arg_index = i,
        };
        memrefs[i].spec.byte_offset = request->offsets[i];
    }
    for (uint32_t i = 0;
         i < DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT; ++i) {
        scalars[i] = (struct lingqu_shmem_pto_scalar_desc) {
            .arg_index = DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT + i,
            .dtype = 8,
            .value = description.scalars[i],
        };
    }
    operation = calloc(1, sizeof(*operation));
    if (!operation) return -ENOMEM;
    *operation_out = operation;
    direct = (struct lingqu_model_direct_request) {
        .compute = request->compute,
        .endpoint = request->endpoint,
        .op_id = request->op_id,
        .request_id = request->request_id,
        .callable_id = DEEPSEEK_V4_FLASH_MANAGED_DIRECT_CALLABLE_ID,
        .artifact_fingerprint = request->artifact_fingerprint,
        .requester_cna = request->requester_cna,
        .memrefs = memrefs,
        .memref_count = DEEPSEEK_V4_FLASH_MANAGED_DIRECT_TENSOR_COUNT,
        .scalars = scalars,
        .scalar_count = DEEPSEEK_V4_FLASH_MANAGED_DIRECT_SCALAR_COUNT,
        .metadata = request->metadata,
        .metadata_bytes = request->metadata_bytes,
        .metadata_iova = request->metadata_iova,
    };
    return lingqu_model_direct_prepare(&direct, &operation->direct);
}

int deepseek_v4_flash_managed_direct_submit(
    struct deepseek_v4_flash_managed_direct_operation *operation)
{
    if (!operation || !operation->direct) return -EINVAL;
    return lingqu_model_direct_submit(operation->direct);
}

int deepseek_v4_flash_managed_direct_poll(
    struct deepseek_v4_flash_managed_direct_operation *operation,
    struct lingqu_shmem_pto_completion *completion_out)
{
    if (!operation || !operation->direct) return -EINVAL;
    return lingqu_model_direct_poll(operation->direct, completion_out);
}

int deepseek_v4_flash_managed_direct_release(
    struct deepseek_v4_flash_managed_direct_operation **operation_ptr)
{
    struct deepseek_v4_flash_managed_direct_operation *operation;
    int rc;

    if (!operation_ptr || !*operation_ptr) return -EINVAL;
    operation = *operation_ptr;
    if (operation->direct) {
        rc = lingqu_model_direct_release(&operation->direct);
        if (rc) return rc;
    }
    free(operation);
    *operation_ptr = NULL;
    return 0;
}
