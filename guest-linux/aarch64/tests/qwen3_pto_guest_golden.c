/* SPDX-License-Identifier: MIT */
/* Reuse the checked fake Memory Service provider, retaining the real lease,
 * memref and dispatch-wire implementations. Only physical/MMIO calls are fake. */
#define main memory_service_base_test
#include "lingqu_shmem_mem_service_golden.c"
#undef main
#include "qwen3_pto_guest.h"

static struct fake_backend backend;
static void *metadata_page;
static int endpoint_error, completion_error, endpoint_closed;
int test_qwen3_physical(void *address, uint64_t *physical);
#define lingqu_shmem_sim_phys_for_virt test_qwen3_physical
#include "../libs/lingqu_shmem_pto/qwen3_pto_guest.c"
#undef lingqu_shmem_sim_phys_for_virt

int test_qwen3_physical(void *address, uint64_t *physical)
{
    metadata_page = address;
    *physical = (uint64_t)(uintptr_t)address;
    return 0;
}

int lingqu_shmem_mem_service_open(struct lingqu_shmem_mem_service_context **out)
{
    return lingqu_shmem_mem_service_context_create_for_backend(&fake_ops, &backend, false, out);
}

int lingqu_shmem_pto_endpoint_open(struct lingqu_shmem_pto_endpoint **out,
                                  struct lingqu_shmem_pto_endpoint_info *info)
{
    *out = (struct lingqu_shmem_pto_endpoint *)&backend;
    memset(info, 0, sizeof(*info));
    return 0;
}

void lingqu_shmem_pto_endpoint_close(struct lingqu_shmem_pto_endpoint *endpoint)
{
    (void)endpoint;
    endpoint_closed++;
}

bool lingqu_shmem_pto_completion_succeeded(const struct lingqu_shmem_pto_completion *c)
{
    return c && c->source == 1 && c->status == 1;
}

int lingqu_shmem_pto_endpoint_submit(struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot, uint64_t timeout,
    struct lingqu_shmem_pto_completion *completion)
{
    const LingquPtoDispatchControlV2 *control = metadata_page;
    (void)endpoint; (void)slot; (void)timeout;
    CHECK(control->callable_id == 3 && control->memref_count == 6 && control->scalar_count == 12);
    CHECK(backend.release_count == 0);
    if (endpoint_error) return endpoint_error;
    completion->source = 1;
    completion->status = completion_error ? 2 : 1;
    return 0;
}

int main(void)
{
    struct qwen3_pto_guest_geometry g = {0, 2, 2, 0, 2, 16, 24, 4, 2, 8, 67, 0};
    struct qwen3_pto_guest_operation *op = NULL;
    struct qwen3_pto_guest_output output;
    struct lingqu_shmem_pto_completion completion;
    const uint32_t tokens[] = {66, 31};
    const uint64_t piece[] = {UINT64_C(0xcf741d1743e6e130), 3, UINT64_C(0x61a0c4), 0};
    CHECK(qwen3_pto_sample_text_checksum(0, 264, piece) == UINT64_C(0xd8f74df298b6be59));
    CHECK(qwen3_pto_sample_text_checksum(1, 264, piece) !=
          qwen3_pto_sample_text_checksum(0, 264, piece));
    float scale = 1.0f / sqrtf(8.0f);
    memcpy(&g.scale_bits, &scale, 4);
    CHECK(memory_service_base_test() == 0);
    g.vocab = 66;
    CHECK(qwen3_pto_guest_prepare(&g, NULL, NULL, tokens, 1, 2, 3, &op) == -ERANGE);
    CHECK(!op && !backend.acquire_local_count);
    g.vocab = 67;
    backend.kv_error = -ENOSPC;
    CHECK(qwen3_pto_guest_prepare(&g, NULL, NULL, tokens, 1, 2, 3, &op) == -ENOSPC);
    CHECK(!op && backend.acquire_local_kv_count == 1);
    CHECK(backend.acquire_local_count == 4 && backend.release_count == 4);
    for (uint32_t end = 1; end <= 2; ++end) {
        memset(&backend, 0, sizeof(backend));
        g.end = end;
        CHECK(qwen3_pto_guest_prepare(&g, NULL, NULL, tokens, 1, 2, 3, &op) == 0);
        const LingquShmemMemrefV1 *wire = (const LingquShmemMemrefV1 *)
            ((const uint8_t *)metadata_page + sizeof(LingquPtoDispatchControlV2));
        CHECK(wire[5].access == (end == g.layers ?
              LINGQU_SHMEM_ACCESS_WRITE : LINGQU_SHMEM_ACCESS_READ));
        CHECK(backend.acquire_local_kv_count == 1);
        CHECK(wire[4].byte_offset == op->buffers[4].backing_offset);
        CHECK(wire[4].byte_offset % 256 == 0);
        CHECK(wire[4].access == LINGQU_SHMEM_ACCESS_READ_WRITE);
        CHECK(qwen3_pto_guest_release(op) == 0);
    }
    for (unsigned int mode = 0; mode < 3; ++mode) {
        memset(&backend, 0, sizeof(backend));
        endpoint_closed = 0;
        endpoint_error = mode == 2 ? -ETIMEDOUT : 0;
        completion_error = mode == 1;
        CHECK(qwen3_pto_guest_prepare(&g, NULL, NULL, tokens, 1, 2, 3, &op) == 0);
        CHECK(backend.acquire_local_count == 6 && backend.release_count == 0);
        CHECK(backend.acquire_local_kv_count == 1);
        CHECK(lingqu_shmem_mem_service_release(op->leases[0]) == -EBUSY);
        int rc = qwen3_pto_guest_submit(op, 100, &output, &completion);
        CHECK(rc == (mode == 0 ? 0 : mode == 1 ? -EIO : -ETIMEDOUT));
        if (mode == 0) {
            CHECK(output.hidden.data && output.hidden.len == 64);
            CHECK(output.kv.data && output.kv.len == 592);
            CHECK(output.kv.backing_offset == op->buffers[4].backing_offset);
            CHECK(output.logits.data && output.logits.len == 268);
            uint64_t header[5];
            memcpy(header, output.kv.data, sizeof(header));
            CHECK(header[0] == 0 && header[1] == 2 && header[4] == 16);
        } else CHECK(!output.hidden.data && !output.kv.data && !output.logits.data);
        CHECK(qwen3_pto_guest_release(op) == (mode == 2 ? -EBUSY : 0));
        CHECK(backend.release_count == (mode == 2 ? 0 : 6));
        CHECK(endpoint_closed == (mode == 2 ? 0 : 1));
        /* The unresolved timeout case deliberately retains its mappings until
         * process exit. This test does not claim timeout recovery support. */
    }
    puts("qwen3_pto_guest_golden=pass");
    return 0;
}
