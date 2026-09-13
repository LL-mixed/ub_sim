/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LINQU_GVA_MANAGER_H
#define LINQU_GVA_MANAGER_H

#include <stdbool.h>
#include <ub/obmm.h>
#include <ub/gsva.h>

/* These APIs borrow fd. Failed commands retain kernel outputs for recovery. */
int gva_manager_register_aperture(
    int fd, const struct obmm_cmd_gsva_aperture *request,
    struct obmm_cmd_gsva_aperture *observed);
int gva_manager_allocate_segment(
    int fd, struct obmm_cmd_gsva_alloc_segment_v1 *command);
int gva_manager_query_segment(
    int fd, struct obmm_cmd_gsva_query_segment_v1 *command);
int gva_manager_retire_segment(
    int fd, struct obmm_cmd_gsva_retire_segment_v1 *command);
int gva_manager_export_segment(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment,
    struct obmm_cmd_export *export_out);
/* 0: created, 1: kernel-confirmed no backing, -1: unknown; failures set errno. */
int gva_manager_export_segment_checked(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment, bool fast,
    struct obmm_cmd_export *export_out);
int gva_manager_unexport_segment(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment,
    const struct obmm_cmd_export *exported);

#endif
