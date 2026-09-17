/* SPDX-License-Identifier: GPL-2.0 */
#include "gva_manager.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int fail(int error)
{
    errno = error;
    return -1;
}

static int power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static int valid_range(uint64_t base, uint64_t size, uint64_t alignment)
{
    return power_of_two(alignment) && size && size <= UINT64_MAX - base &&
           !(base & (alignment - 1));
}

static int valid_descriptor(const struct obmm_gsva_segment_desc_v1 *desc)
{
    const uint32_t required = OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
                              OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED |
                              OBMM_GSVA_SEG_F_ACTIVE;
    long page_size = sysconf(_SC_PAGESIZE);

    return page_size > 0 && desc->version == OBMM_GSVA_ABI_VERSION &&
           (desc->flags & required) == required &&
           !(desc->flags & OBMM_GSVA_SEG_F_RETIRED) &&
           desc->segment_id && desc->epoch && desc->home_cna &&
           desc->home_va && desc->token_id && desc->token_value &&
           desc->node_count && desc->owner_node_id < desc->node_count &&
           valid_range(desc->home_va, desc->size, (uint64_t)page_size) &&
           !(desc->size & ((uint64_t)page_size - 1));
}

static int valid_aperture_request(
    const struct obmm_cmd_gsva_aperture *request, long page_size)
{
    return request && page_size > 0 &&
           valid_range(request->base, request->size, (uint64_t)page_size) &&
           !(request->size & ((uint64_t)page_size - 1)) &&
           request->generation && request->node_count &&
           request->node_id < request->node_count;
}

int gva_manager_validate_aperture(
    int fd, const struct obmm_cmd_gsva_aperture *expected,
    struct obmm_cmd_gsva_aperture *observed)
{
    long page_size = sysconf(_SC_PAGESIZE);

    if (!observed || !valid_aperture_request(expected, page_size))
        return fail(EINVAL);
    memset(observed, 0, sizeof(*observed));
    if (ioctl(fd, OBMM_CMD_GSVA_APERTURE_QUERY, observed) != 0)
        return -1;
    if (!(observed->flags & OBMM_GSVA_APERTURE_F_ACTIVE) ||
        observed->base != expected->base || observed->size != expected->size ||
        observed->generation != expected->generation ||
        observed->node_id != expected->node_id ||
        observed->node_count != expected->node_count)
        return fail(EPROTO);
    return 0;
}

int gva_manager_register_aperture(
    int fd, const struct obmm_cmd_gsva_aperture *request,
    struct obmm_cmd_gsva_aperture *observed)
{
    struct obmm_cmd_gsva_aperture copy;
    long page_size = sysconf(_SC_PAGESIZE);

    if (!observed || !valid_aperture_request(request, page_size))
        return fail(EINVAL);
    copy = *request;
    copy.flags |= OBMM_GSVA_APERTURE_F_ACTIVE;
    if (ioctl(fd, OBMM_CMD_GSVA_APERTURE_REGISTER, &copy) != 0)
        return -1;
    return gva_manager_validate_aperture(fd, &copy, observed);
}

int gva_manager_allocate_segment(
    int fd, struct obmm_cmd_gsva_alloc_segment_v1 *command)
{
    struct obmm_cmd_gsva_alloc_segment_v1 request;
    const struct obmm_gsva_segment_desc_v1 *desc;
    uint64_t alignment;
    long page_size = sysconf(_SC_PAGESIZE);

    if (!command || page_size <= 0)
        return fail(EINVAL);
    request = *command;
    alignment = request.alignment ? request.alignment : (uint64_t)page_size;
    if (request.version != OBMM_GSVA_ABI_VERSION ||
        alignment < (uint64_t)page_size ||
        !valid_range(request.requested_home_va, request.size, alignment) ||
        (request.size & ((uint64_t)page_size - 1)))
        return fail(EINVAL);
    if (ioctl(fd, OBMM_CMD_GSVA_ALLOC_SEGMENT, command) != 0)
        return -1;
    desc = &command->desc;
    if (!valid_descriptor(desc) || desc->size != request.size ||
        (desc->home_va & (alignment - 1)) ||
        (request.requested_home_va &&
         desc->home_va != request.requested_home_va) ||
        desc->owner_node_id != request.home_node_id ||
        desc->cache_policy != request.cache_policy ||
        desc->access_flags != request.access_flags ||
        (request.requested_p_tag != OBMM_GSVA_P_TAG_AUTO &&
         desc->p_tag != request.requested_p_tag))
        return fail(EPROTO);
    return 0;
}

int gva_manager_query_segment(
    int fd, struct obmm_cmd_gsva_query_segment_v1 *command)
{
    uint64_t id, address;

    if (!command || command->version != OBMM_GSVA_ABI_VERSION ||
        (!command->segment_id && !command->home_va))
        return fail(EINVAL);
    id = command->segment_id;
    address = command->home_va;
    if (ioctl(fd, OBMM_CMD_GSVA_QUERY_SEGMENT, command) != 0)
        return -1;
    if (!valid_descriptor(&command->desc) ||
        (id && command->desc.segment_id != id) ||
        (address && command->desc.home_va != address))
        return fail(EPROTO);
    return 0;
}

int gva_manager_enumerate_segments(
    int fd, struct obmm_cmd_gsva_enumerate_v1 *command)
{
#ifndef OBMM_CMD_GSVA_ENUMERATE
    /* Standalone SDK consumers can build against their older UAPI profile.
     * Missing enumeration never becomes an apparently valid empty inventory. */
    (void)fd;
    (void)command;
    return fail(EOPNOTSUPP);
#else
    struct obmm_cmd_gsva_enumerate_v1 reply = {0};
    struct obmm_gsva_segment_desc_v1 active, empty = {0};
    const unsigned char zero_instance[16] = {0};
    uint32_t state;

    if (!command || command->version != OBMM_GSVA_ABI_VERSION ||
        (!command->revision && (command->cursor ||
         memcmp(command->kernel_instance, zero_instance, sizeof(zero_instance)))))
        return fail(EINVAL);
    reply.version = command->version;
    reply.revision = command->revision;
    reply.cursor = command->cursor;
    memcpy(reply.kernel_instance, command->kernel_instance, sizeof(reply.kernel_instance));
    if (ioctl(fd, OBMM_CMD_GSVA_ENUMERATE, &reply))
        return -1;
    if (reply.version != OBMM_GSVA_ABI_VERSION || !reply.revision ||
        !memcmp(reply.kernel_instance, zero_instance, sizeof(zero_instance)) ||
        reply.reserved || reply.reserved2 ||
        (command->revision && (reply.revision != command->revision ||
         memcmp(reply.kernel_instance, command->kernel_instance, sizeof(reply.kernel_instance)))))
        return fail(EPROTO);
    if (reply.flags == OBMM_GSVA_ENUM_END) {
        if (reply.cursor != command->cursor || reply.resource_flags || reply.export_mem_id ||
            reply.export_token_id || reply.aperture_generation ||
            memcmp(&reply.desc, &empty, sizeof(empty)))
            return fail(EPROTO);
    } else if (reply.flags == OBMM_GSVA_ENUM_ENTRY) {
        state = reply.desc.flags & (OBMM_GSVA_SEG_F_ACTIVE | OBMM_GSVA_SEG_F_RETIRED);
        active = reply.desc;
        active.flags = (active.flags & ~OBMM_GSVA_SEG_F_RETIRED) | OBMM_GSVA_SEG_F_ACTIVE;
        if ((state != OBMM_GSVA_SEG_F_ACTIVE && state != OBMM_GSVA_SEG_F_RETIRED) ||
            !valid_descriptor(&active) || reply.cursor <= command->cursor ||
            reply.cursor != (reply.desc.segment_id & ((1ULL << 48) - 1)) ||
            (reply.resource_flags & ~(OBMM_GSVA_RESOURCE_ADDRESS_RESERVED |
                                      OBMM_GSVA_RESOURCE_EXPORT_BUSY)) ||
            (!!reply.export_mem_id != !!reply.export_token_id) ||
            (state == OBMM_GSVA_SEG_F_ACTIVE &&
             !(reply.resource_flags & OBMM_GSVA_RESOURCE_ADDRESS_RESERVED)) ||
            (state == OBMM_GSVA_SEG_F_RETIRED &&
             (reply.resource_flags || reply.export_mem_id)))
            return fail(EPROTO);
    } else {
        return fail(EPROTO);
    }
    *command = reply;
    return 0;
#endif
}

int gva_manager_export_segment(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment,
    struct obmm_cmd_export *export_out)
{
    struct obmm_cmd_gsva_query_segment_v1 query = {0};
    const struct obmm_gsva_segment_desc_v1 *actual = &query.desc;

    if (!export_out)
        return fail(EINVAL);
    memset(export_out, 0, sizeof(*export_out));
    if (!segment || !valid_descriptor(segment))
        return fail(EINVAL);
    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = segment->segment_id;
    query.home_va = segment->home_va;
    if (gva_manager_query_segment(fd, &query) != 0)
        return -1;
    if (actual->flags != segment->flags || actual->size != segment->size ||
        actual->epoch != segment->epoch || actual->home_cna != segment->home_cna ||
        actual->owner_node_id != segment->owner_node_id ||
        actual->node_count != segment->node_count ||
        actual->cache_policy != segment->cache_policy ||
        actual->p_tag != segment->p_tag || actual->access_flags != segment->access_flags ||
        actual->token_id != segment->token_id || actual->token_value != segment->token_value)
        return fail(ESTALE);
    export_out->length = 1;
    export_out->size[0] = segment->size;
    export_out->flags = OBMM_EXPORT_FLAG_ALLOW_MMAP | OBMM_EXPORT_FLAG_GSVA_FIXED_UBA;
    export_out->uba = segment->home_va;
    if (ioctl(fd, OBMM_CMD_EXPORT, export_out) != 0)
        return -1;
    if (export_out->uba != segment->home_va ||
        export_out->size[0] != segment->size ||
        !export_out->mem_id || !export_out->tokenid)
        return fail(EPROTO);
    return 0;
}

int gva_manager_export_segment_checked(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment, bool fast,
    struct obmm_cmd_export *export_out)
{
    struct obmm_cmd_gsva_export_segment_v1 command = {0};
    int result, saved_errno;

    if (!export_out)
        return fail(EINVAL);
    memset(export_out, 0, sizeof(*export_out));
    if (!segment || !valid_descriptor(segment))
        return fail(EINVAL);
    command.version = OBMM_GSVA_ABI_VERSION;
    command.flags = fast ? OBMM_GSVA_EXPORT_F_FAST : 0;
    command.segment = *segment;
    result = ioctl(fd, OBMM_CMD_GSVA_EXPORT_SEGMENT, &command);
    saved_errno = errno;
    /* Preserve even partial or contradictory kernel outputs for reconciliation. */
    export_out->length = 1;
    export_out->flags = OBMM_EXPORT_FLAG_ALLOW_MMAP | OBMM_EXPORT_FLAG_GSVA_FIXED_UBA |
                        (fast ? OBMM_EXPORT_FLAG_FAST : 0);
    export_out->mem_id = command.export_mem_id;
    export_out->tokenid = command.export_token_id;
    export_out->uba = command.export_uba;
    export_out->size[0] = command.export_size;
    if (result)
        return fail(saved_errno);
    if (command.version != OBMM_GSVA_ABI_VERSION || command.reserved ||
        command.flags != (fast ? OBMM_GSVA_EXPORT_F_FAST : 0) ||
        memcmp(&command.segment, segment, sizeof(*segment)))
        return fail(EPROTO);
    if (command.outcome == OBMM_GSVA_EXPORT_CREATED) {
        if (command.error || !command.export_mem_id || !command.export_token_id ||
            command.export_uba != segment->home_va || command.export_size != segment->size)
            return fail(EPROTO);
        return 0;
    }
    if (command.outcome == OBMM_GSVA_EXPORT_NO_BACKING) {
        if (command.error <= 0 || command.error > 4095 || command.export_mem_id ||
            command.export_token_id || command.export_uba || command.export_size)
            return fail(EPROTO);
        errno = command.error;
        return 1;
    }
    if (command.outcome != OBMM_GSVA_EXPORT_UNKNOWN ||
        command.error <= 0 || command.error > 4095)
        return fail(EPROTO);
    return fail(command.error);
}

int gva_manager_unexport_segment(
    int fd, const struct obmm_gsva_segment_desc_v1 *segment,
    const struct obmm_cmd_export *exported)
{
    struct obmm_cmd_gsva_query_segment_v1 query = {0};
    struct obmm_cmd_unexport command = {0};

    if (!segment || !exported || !valid_descriptor(segment) ||
        !exported->mem_id || !exported->tokenid || exported->length != 1 ||
        exported->uba != segment->home_va || exported->size[0] != segment->size ||
        !(exported->flags & OBMM_EXPORT_FLAG_GSVA_FIXED_UBA))
        return fail(EINVAL);
    query.version = OBMM_GSVA_ABI_VERSION;
    query.segment_id = segment->segment_id;
    query.home_va = segment->home_va;
    if (gva_manager_query_segment(fd, &query))
        return -1;
    if (memcmp(&query.desc, segment, sizeof(*segment)))
        return fail(ESTALE);
    command.mem_id = exported->mem_id;
    return ioctl(fd, OBMM_CMD_UNEXPORT, &command);
}

int gva_manager_retire_segment(
    int fd, struct obmm_cmd_gsva_retire_segment_v1 *command)
{
    uint64_t epoch;

    if (!command || command->version != OBMM_GSVA_ABI_VERSION ||
        !command->segment_id || !command->epoch)
        return fail(EINVAL);
    epoch = command->epoch;
    if (ioctl(fd, OBMM_CMD_GSVA_RETIRE_SEGMENT, command) != 0)
        return -1;
    if (command->status != OBMM_GSVA_RETIRE_COMMITTED || command->error ||
        command->committed_epoch != epoch)
        return fail(EPROTO);
    return 0;
}
