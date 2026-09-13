/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_MANAGED_H
#define LINGQU_SHMEM_PTO_MANAGED_H

#include <stdbool.h>

/* Deployment-owned, model-free two-node managed mapping acceptance mode. */
int lingqu_shmem_pto_managed_main(const char *config_path, bool validate_only);

#endif
