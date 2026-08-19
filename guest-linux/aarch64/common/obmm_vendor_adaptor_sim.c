/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Simulator vendor adaptor for libobmm.  Replaces the hardware-specific
 * vendor_adaptor.c from vendor/obmm at link time: the ub_sim guest has no
 * ub_bus_controller sysfs, and exports never carry EIDs.
 */

#include <libobmm.h>
#include "vendor_adaptor.h"

int vendor_adapt_export(struct obmm_mem_desc *desc, const void **vendor_info,
            uint16_t *vendor_len, int *numa)
{
    (void)desc;
    *vendor_info = NULL;
    *vendor_len = 0;
    *numa = 0;
    return 0;
}

void free_vendor_info(void *vendor_info)
{
    (void)vendor_info;
}

int vendor_fixup_import_cmd(struct obmm_cmd_import *cmd)
{
    (void)cmd;
    return 0;
}

void vendor_cleanup_import_cmd(struct obmm_cmd_import *cmd)
{
    (void)cmd;
}

int vendor_fixup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
    (void)cmd;
    return 0;
}

void vendor_cleanup_preimport_cmd(struct obmm_cmd_preimport *cmd)
{
    (void)cmd;
}
