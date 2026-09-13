/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_MANAGED_H
#define LINGQU_SHMEM_PTO_MANAGED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lingqu_shmem_pto_wire_result;

/* Acceptance-only metadata mutation support shared with the legacy workload. */
int lingqu_shmem_pto_refresh_fault_metadata_crc(
    uint8_t *metadata, size_t metadata_capacity, uint64_t metadata_iova,
    struct lingqu_shmem_pto_wire_result *wire_result);

/* Deployment-owned, model-free two-node managed mapping acceptance mode. */
int lingqu_shmem_pto_managed_main(const char *config_path, bool validate_only);

#endif
