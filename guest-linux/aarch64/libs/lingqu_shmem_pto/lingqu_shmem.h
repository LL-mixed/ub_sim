/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_H
#define LINGQU_SHMEM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct lingqu_shmem_region;
struct lingqu_shmem_memref;

#define LINGQU_SHMEM_MAX_RANK 5u

enum lingqu_shmem_access {
    LINGQU_SHMEM_ACCESS_READ = 1u << 0,
    LINGQU_SHMEM_ACCESS_WRITE = 1u << 1,
    LINGQU_SHMEM_ACCESS_READ_WRITE =
        LINGQU_SHMEM_ACCESS_READ | LINGQU_SHMEM_ACCESS_WRITE,
};

struct lingqu_shmem_memref_spec {
    uint64_t byte_offset;
    /* Storage span covered by shape/strides, including any padding holes. */
    uint64_t byte_length;
    uint32_t rank;
    uint16_t dtype;
    uint8_t access;
    uint32_t shape[LINGQU_SHMEM_MAX_RANK];
    /* Element strides; overlapping multi-element dimensions are rejected. */
    uint32_t strides[LINGQU_SHMEM_MAX_RANK];
};

int lingqu_shmem_memref_create(
    struct lingqu_shmem_region *region,
    const struct lingqu_shmem_memref_spec *spec,
    struct lingqu_shmem_memref **memref_out);

int lingqu_shmem_memref_destroy(struct lingqu_shmem_memref *memref);

#ifdef __cplusplus
}
#endif

#endif
