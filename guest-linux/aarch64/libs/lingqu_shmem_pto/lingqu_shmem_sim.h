/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_SIM_H
#define LINGQU_SHMEM_SIM_H

#include <stddef.h>
#include <stdint.h>

#include "lingqu_shmem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Simulator-only region attachment.  Guest applications obtain these fields
 * from an active OBMM import/map registration; the public memref API never
 * exposes them.
 */
struct lingqu_shmem_sim_region_desc {
    void *mapped_addr;
    uint64_t mapped_length;
    uint64_t ub_gm_addr;
    uint64_t opaque_mapping_ref;
};

int lingqu_shmem_sim_region_create(
    const struct lingqu_shmem_sim_region_desc *desc,
    struct lingqu_shmem_region **region_out);

int lingqu_shmem_sim_region_destroy(struct lingqu_shmem_region *region);

int lingqu_shmem_sim_phys_for_virt(void *address, uint64_t *physical_out);

#ifdef __cplusplus
}
#endif

#endif
