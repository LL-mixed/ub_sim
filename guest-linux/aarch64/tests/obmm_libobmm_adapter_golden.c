/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Byte-equivalence harness for the obmm_common.h -> libobmm adapter layer.
 * Compiled with -Wl,--wrap=ioctl,--wrap=open so both the legacy raw-ioctl
 * path and the libobmm path are intercepted and compared against golden
 * obmm_cmd_* expectations.  Exits non-zero on any mismatch.
 *
 * GOLDEN_LEGACY=1 env switches the export cmd.length expectation from the
 * libobmm value (OBMM_MAX_LOCAL_NUMA_NODES) to the legacy raw-ioctl value
 * (1) so the same harness can validate the baseline before the rewrite.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ub/obmm.h>
#include "obmm_common.h"

static int g_failures;

#define CHECK(cond, name) do { \
    if (cond) { printf("ok %s\n", (name)); } \
    else { printf("FAIL %s\n", (name)); g_failures++; } \
} while (0)

extern int __real_ioctl(int, unsigned long, ...);
extern int __real_open(const char *, int, ...);

static struct obmm_cmd_export g_last_export;
static struct obmm_cmd_unexport g_last_unexport;
static struct obmm_cmd_import g_last_import;
static struct obmm_cmd_unimport g_last_unimport;
static int g_ioctl_count;

/*
 * obmm_cmd_import.priv is a pointer that in the legacy path targets a
 * stack local of obmm_do_import() which dies before the assertions run.
 * Deep-copy the priv blob here so it stays valid for inspection.
 */
static uint8_t g_import_priv_copy[OBMM_MAX_PRIV_LEN];

int __wrap_open(const char *path, int flags, ...)
{
    if (strcmp(path, "/dev/obmm") == 0)
        return 42;
    return __real_open(path, flags);
}

int __wrap_ioctl(int fd, unsigned long req, ...)
{
    void *arg;
    va_list ap;
    (void)fd;
    va_start(ap, req);
    arg = va_arg(ap, void *);
    va_end(ap);
    g_ioctl_count++;

    if (req == (unsigned long)OBMM_CMD_EXPORT) {
        struct obmm_cmd_export *cmd = arg;
        g_last_export = *cmd;
        /* kernel-side out params */
        cmd->mem_id = 0x1122334455667788ULL;
        cmd->uba = 0x2000000ULL;
        cmd->tokenid = 0x5a5a;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_UNEXPORT) {
        g_last_unexport = *(struct obmm_cmd_unexport *)arg;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_IMPORT) {
        struct obmm_cmd_import *cmd = arg;
        g_last_import = *cmd;
        if (cmd->priv != NULL && cmd->priv_len > 0 &&
            cmd->priv_len <= sizeof(g_import_priv_copy)) {
            memcpy(g_import_priv_copy, cmd->priv, cmd->priv_len);
        }
        cmd->mem_id = 0x99aabbccddeeff00ULL;
        return 0;
    }
    if (req == (unsigned long)OBMM_CMD_UNIMPORT) {
        g_last_unimport = *(struct obmm_cmd_unimport *)arg;
        return 0;
    }
    errno = ENOTTY;
    return -1;
}

static const void *import_priv_ptr(void)
{
    return g_import_priv_copy;
}

int main(void)
{
    struct obmm_helpers_meta meta = {0};
    struct obmm_helpers_meta peer = {0};
    uint64_t import_mem_id = 0;
    int fd = obmm_open_device();
    int i;

    CHECK(fd == 42, "open(/dev/obmm)");

    /* ---- export ---- */
    CHECK(obmm_do_export(fd, &meta, 0x100000) == 0, "do_export rc");
    CHECK(meta.export_mem_id == 0x1122334455667788ULL, "export mem_id");
    CHECK(meta.remote_uba == 0x2000000ULL, "export uba");
    CHECK(meta.token_id == 0x5a5a, "export tokenid");
    CHECK(meta.size == 0x100000, "export size");
    CHECK(g_last_export.size[0] == 0x100000, "cmd size[0]");
    {
        int rest_zero = 1;
        for (i = 1; i < OBMM_MAX_LOCAL_NUMA_NODES; i++)
            if (g_last_export.size[i] != 0) { rest_zero = 0; break; }
        CHECK(rest_zero, "cmd size[1..] zero");
    }
    CHECK(g_last_export.flags == OBMM_EXPORT_FLAG_ALLOW_MMAP, "cmd flags");
    CHECK(g_last_export.pxm_numa == 0, "cmd pxm_numa");
    CHECK(g_last_export.uba == 0, "cmd in uba zero");
    {
        const char *legacy = getenv("GOLDEN_LEGACY");
        unsigned long expect_len = (legacy && legacy[0] == '1')
            ? 1UL : (unsigned long)OBMM_MAX_LOCAL_NUMA_NODES;
        CHECK((unsigned long)g_last_export.length == expect_len,
              "cmd length (libobmm semantics)");
    }

    /* ---- import v1 ---- */
    peer.remote_uba = 0x2000000ULL;
    peer.size = 0x100000;
    peer.token_id = 0x5a5a;
    peer.export_cna = 3;
    CHECK(obmm_do_import(fd, &peer, 7, 0x480000000ULL, 0xa1b2,
                         &import_mem_id) == 0, "do_import rc");
    CHECK(import_mem_id == 0x99aabbccddeeff00ULL, "import mem_id");
    CHECK(g_last_import.addr == 0x480000000ULL, "cmd addr");
    CHECK(g_last_import.length == 0x100000, "cmd length");
    CHECK(g_last_import.tokenid == 0x5a5a, "cmd tokenid");
    CHECK(g_last_import.scna == 7, "cmd scna");
    CHECK(g_last_import.dcna == 3, "cmd dcna");
    CHECK(g_last_import.flags == OBMM_IMPORT_FLAG_ALLOW_MMAP, "cmd flags");
    CHECK(g_last_import.base_dist == 0, "cmd base_dist");
    CHECK(g_last_import.numa_id == 0, "cmd numa_id");
    {
        const struct obmm_sim_dec_import_priv_v1 *priv = import_priv_ptr();
        CHECK(g_last_import.priv_len == sizeof(*priv), "cmd priv_len");
        CHECK(priv->magic == OBMM_SIM_DEC_PRIV_MAGIC, "priv magic");
        CHECK(priv->version == OBMM_SIM_DEC_PRIV_VER_1, "priv version");
        CHECK(priv->len == sizeof(*priv), "priv len");
        CHECK(priv->remote_uba == 0x2000000ULL, "priv remote_uba");
        CHECK(priv->token_value == 0xa1b2, "priv token_value");
    }

    /* ---- import v2 ---- */
    CHECK(obmm_do_import_v2_epoch(fd, &peer, 7, 0x480000000ULL, 0xa1b2,
            OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
            OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY,
            OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI,
            11, 22, 33, 44, OBMM_SIM_DEC_ACCESS_READ_ONLY,
            0x777, 9, 0x600000, 0x600000, 0x40, &import_mem_id) == 0,
          "do_import_v2 rc");
    {
        const struct obmm_sim_dec_import_priv_v2 *priv = import_priv_ptr();
        CHECK(g_last_import.priv_len == sizeof(*priv), "v2 priv_len");
        CHECK(priv->magic == OBMM_SIM_DEC_PRIV_MAGIC, "v2 magic");
        CHECK(priv->version == OBMM_SIM_DEC_PRIV_VER_2, "v2 version");
        CHECK(priv->remote_uba == 0x2000000ULL, "v2 remote_uba");
        CHECK(priv->token_value == 0xa1b2, "v2 token_value");
        CHECK(priv->map_source == OBMM_SIM_DEC_MAP_SOURCE_GVA_MANAGER,
              "v2 map_source");
        CHECK(priv->address_profile ==
              OBMM_SIM_DEC_ADDRESS_PROFILE_GSVA_IDENTITY, "v2 profile");
        CHECK(priv->cache_policy ==
              OBMM_SIM_DEC_CACHE_POLICY_DIRECTORY_MESI, "v2 cache_policy");
        CHECK(priv->vmid == 11, "v2 vmid");
        CHECK(priv->asid == 22, "v2 asid");
        CHECK(priv->tid == 33, "v2 tid");
        CHECK(priv->p_tag == 44, "v2 p_tag");
        CHECK(priv->access_flags == OBMM_SIM_DEC_ACCESS_READ_ONLY,
              "v2 access_flags");
        CHECK(priv->gva_id == 0x777, "v2 gva_id");
        CHECK(priv->segment_id == 0x777, "v2 segment_id");
        CHECK(priv->epoch == 9, "v2 epoch");
        CHECK(priv->local_va == 0x600000, "v2 local_va");
        CHECK(priv->home_va == 0x600000, "v2 home_va");
        CHECK(priv->pte_offset == 0x40, "v2 pte_offset");
    }

    /* ---- unexport / unimport ---- */
    CHECK(obmm_do_unexport(fd, 0x1122334455667788ULL) == 0, "unexport rc");
    CHECK(g_last_unexport.mem_id == 0x1122334455667788ULL, "unexport mem_id");
    CHECK(obmm_do_unimport(fd, 0x99aabbccddeeff00ULL) == 0, "unimport rc");
    CHECK(g_last_unimport.mem_id == 0x99aabbccddeeff00ULL, "unimport mem_id");

    if (g_failures == 0)
        printf("adapter-golden: ok (%d ioctls)\n", g_ioctl_count);
    else
        printf("adapter-golden: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
