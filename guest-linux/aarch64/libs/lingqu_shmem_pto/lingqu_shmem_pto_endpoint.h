/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_ENDPOINT_H
#define LINGQU_SHMEM_PTO_ENDPOINT_H

#include <stdbool.h>
#include <stdint.h>

#include <linqu_shmem_pto_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LINGQU_SHMEM_PTO_COMPLETION_BYTES 64u
#define LINGQU_SHMEM_PTO_COMPLETION_CODE_BYTES 45u

struct lingqu_shmem_pto_endpoint;

struct lingqu_shmem_pto_endpoint_info {
    char resource_path[128];
    uint64_t root_version;
    uint64_t default_segment;
    uint32_t cmdq_depth;
    uint32_t cq_depth;
};

struct lingqu_shmem_pto_completion {
    uint64_t op_id;
    uint64_t finished_at;
    uint8_t task_marker;
    uint8_t source;
    uint8_t status;
    char error_code[LINGQU_SHMEM_PTO_COMPLETION_CODE_BYTES];
};

int lingqu_shmem_pto_completion_decode(
    const uint8_t slot[LINGQU_SHMEM_PTO_COMPLETION_BYTES],
    struct lingqu_shmem_pto_completion *completion);

bool lingqu_shmem_pto_completion_succeeded(
    const struct lingqu_shmem_pto_completion *completion);

int lingqu_shmem_pto_endpoint_open(
    struct lingqu_shmem_pto_endpoint **endpoint_out,
    struct lingqu_shmem_pto_endpoint_info *info_out);

int lingqu_shmem_pto_endpoint_submit(
    struct lingqu_shmem_pto_endpoint *endpoint,
    const LingquPtoDispatchSlotV2 *slot,
    uint64_t timeout_ms,
    struct lingqu_shmem_pto_completion *completion);

void lingqu_shmem_pto_endpoint_close(
    struct lingqu_shmem_pto_endpoint *endpoint);

#ifdef __cplusplus
}
#endif

#endif
