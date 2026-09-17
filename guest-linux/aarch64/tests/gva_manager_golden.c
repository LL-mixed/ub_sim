/* SPDX-License-Identifier: GPL-2.0 */
#include "gva_manager.h"

#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

enum response_mode {
    VALID, IO_ERROR, APERTURE_NODE, APERTURE_GENERATION, BAD_TOKEN,
    BAD_ALIGNMENT, BAD_SIZE, BAD_OWNER, BAD_ID, RETIRED, RETIRE_PENDING,
    RETIRE_EPOCH, RETIRE_ERROR, STALE_TOKEN, EXPORT_ADDRESS, EXPORT_ERROR,
    UNEXPORT_ERROR, CHECKED_EMPTY, CHECKED_MISSING_ERROR, CHECKED_CONTRADICTION,
    CHECKED_UNKNOWN, CHECKED_VERSION, CHECKED_IDENTITY, CHECKED_FLAGS,
    CHECKED_RESERVED, CHECKED_SIZE, CHECKED_OLD_KERNEL, CHECKED_COPY_LOST,
    ALLOC_ENOSPC, ALLOC_OUTPUT_LOST
};
static enum response_mode mode;
static unsigned calls;
static struct obmm_cmd_gsva_aperture aperture;

static struct obmm_gsva_segment_desc_v1 descriptor(void)
{
    struct obmm_gsva_segment_desc_v1 d = {
        .version = OBMM_GSVA_ABI_VERSION,
        .flags = OBMM_GSVA_SEG_F_ACTIVE |
                 OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
                 OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED,
        .segment_id = 73, .home_va = 0x700000000000ULL, .size = 65536,
        .epoch = 19, .home_cna = 7, .owner_node_id = 1, .node_count = 3,
        .token_id = 11, .token_value = 53,
        .access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE,
    };
    if (mode == BAD_TOKEN) d.token_value = 0;
    if (mode == STALE_TOKEN) d.token_value++;
    if (mode == BAD_ALIGNMENT) d.home_va += 4096;
    if (mode == BAD_SIZE) d.size *= 2;
    if (mode == BAD_OWNER) d.owner_node_id = 2;
    if (mode == BAD_ID) d.segment_id++;
    if (mode == RETIRED) d.flags |= OBMM_GSVA_SEG_F_RETIRED;
    return d;
}

/* Executable ioctl boundary test; no OBMM device or payload is simulated. */
int ioctl(int fd, unsigned long op, ...)
{
    va_list args;
    void *argument;

    assert(fd == 42);
    calls++;
    if (mode == IO_ERROR) {
        errno = EBUSY;
        return -1;
    }
    va_start(args, op);
    argument = va_arg(args, void *);
    va_end(args);
    switch (op) {
    case OBMM_CMD_GSVA_EXPORT_SEGMENT: {
        struct obmm_cmd_gsva_export_segment_v1 *command = argument;
        assert(command->version == 1 && !command->reserved);
        assert(!(command->flags & ~OBMM_GSVA_EXPORT_F_FAST));
        assert(command->segment.segment_id == 73 && command->segment.token_value == 53);
        assert(command->segment.home_va == 0x700000000000ULL && command->segment.size == 65536);
        command->outcome = OBMM_GSVA_EXPORT_CREATED;
        command->export_mem_id = 91;
        command->export_token_id = 97;
        command->export_uba = command->segment.home_va;
        command->export_size = command->segment.size;
        if (mode == CHECKED_EMPTY || mode == CHECKED_MISSING_ERROR ||
            mode == CHECKED_CONTRADICTION) {
            command->outcome = OBMM_GSVA_EXPORT_NO_BACKING;
            command->error = mode == CHECKED_MISSING_ERROR ? 0 : ENOMEM;
            command->export_mem_id = mode == CHECKED_CONTRADICTION ? 91 : 0;
            command->export_token_id = 0;
            command->export_uba = 0;
            command->export_size = 0;
        }
        if (mode == CHECKED_UNKNOWN) {
            command->outcome = OBMM_GSVA_EXPORT_UNKNOWN;
            command->error = ENOMEM;
        }
        if (mode == CHECKED_VERSION) command->version++;
        if (mode == CHECKED_IDENTITY) command->segment.token_value++;
        if (mode == CHECKED_FLAGS) command->flags ^= OBMM_GSVA_EXPORT_F_FAST;
        if (mode == CHECKED_RESERVED) command->reserved++;
        if (mode == CHECKED_SIZE) command->export_size++;
        if (mode == CHECKED_OLD_KERNEL || mode == CHECKED_COPY_LOST) {
            errno = mode == CHECKED_OLD_KERNEL ? ENOTTY : EFAULT;
            return -1;
        }
        break;
    }
    case OBMM_CMD_UNEXPORT: {
        const struct obmm_cmd_unexport *command = argument;
        assert(command->mem_id == 91 && command->flags == 0);
        if (mode == UNEXPORT_ERROR) {
            errno = EBUSY;
            return -1;
        }
        break;
    }
    case OBMM_CMD_GSVA_APERTURE_REGISTER:
        aperture = *(struct obmm_cmd_gsva_aperture *)argument;
        break;
    case OBMM_CMD_GSVA_APERTURE_QUERY: {
        struct obmm_cmd_gsva_aperture *query = argument;
        *query = aperture;
        if (mode == APERTURE_NODE) query->node_id++;
        if (mode == APERTURE_GENERATION) query->generation++;
        break;
    }
    case OBMM_CMD_GSVA_ALLOC_SEGMENT:
        if (mode == ALLOC_ENOSPC || mode == ALLOC_OUTPUT_LOST) {
            errno = mode == ALLOC_ENOSPC ? ENOSPC : EFAULT;
            return -1;
        }
        ((struct obmm_cmd_gsva_alloc_segment_v1 *)argument)->desc = descriptor();
        break;
    case OBMM_CMD_GSVA_QUERY_SEGMENT:
        ((struct obmm_cmd_gsva_query_segment_v1 *)argument)->desc = descriptor();
        break;
    case OBMM_CMD_GSVA_RETIRE_SEGMENT: {
        struct obmm_cmd_gsva_retire_segment_v1 *retire = argument;
        retire->status = mode == RETIRE_PENDING ?
            OBMM_GSVA_RETIRE_PENDING_TIMEOUT : OBMM_GSVA_RETIRE_COMMITTED;
        retire->committed_epoch = retire->epoch + (mode == RETIRE_EPOCH);
        retire->error = mode == RETIRE_ERROR;
        break;
    }
    case OBMM_CMD_EXPORT: {
        struct obmm_cmd_export *exported = argument;
        assert(exported->length == 1 && exported->size[0] == 65536);
        assert(exported->uba == 0x700000000000ULL);
        assert(exported->flags ==
               (OBMM_EXPORT_FLAG_ALLOW_MMAP | OBMM_EXPORT_FLAG_GSVA_FIXED_UBA));
        exported->mem_id = 91;
        exported->tokenid = 97;
        if (mode == EXPORT_ADDRESS) exported->uba += 65536;
        if (mode == EXPORT_ERROR) {
            errno = EIO;
            return -1;
        }
        break;
    }
    default:
        assert(0);
    }
    return 0;
}

int main(void)
{
    struct obmm_cmd_gsva_aperture request = {
        .base = 0x700000000000ULL, .size = 16 * 65536, .generation = 29,
        .node_id = 1, .node_count = 3,
    }, observed;
    struct obmm_cmd_gsva_alloc_segment_v1 alloc = {
        .version = OBMM_GSVA_ABI_VERSION, .size = 65536,
        .alignment = 65536, .requested_home_va = 0x700000000000ULL,
        .home_node_id = 1, .requested_p_tag = OBMM_GSVA_P_TAG_AUTO,
        .access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE,
    };
    struct obmm_cmd_gsva_query_segment_v1 query = {
        .version = OBMM_GSVA_ABI_VERSION, .segment_id = 73,
    };
    struct obmm_cmd_gsva_retire_segment_v1 retire = {
        .version = OBMM_GSVA_ABI_VERSION, .segment_id = 73, .epoch = 19,
    };
    struct obmm_gsva_segment_desc_v1 segment;
    struct obmm_cmd_export exported;
    const enum response_mode bad_alloc[] = {
        BAD_TOKEN, BAD_ALIGNMENT, BAD_SIZE, BAD_OWNER, RETIRED
    };
    const enum response_mode bad_retire[] = {
        RETIRE_PENDING, RETIRE_EPOCH, RETIRE_ERROR
    };
    unsigned before;
    size_t i;

    assert(gva_manager_register_aperture(42, &request, &observed) == 0);
    assert(!(request.flags & OBMM_GSVA_APERTURE_F_ACTIVE));
    assert(observed.flags & OBMM_GSVA_APERTURE_F_ACTIVE);
    before = calls;
    assert(gva_manager_validate_aperture(42, &request, &observed) == 0);
    assert(calls == before + 1);
    mode = APERTURE_NODE;
    before = calls;
    assert(gva_manager_validate_aperture(42, &request, &observed) == -1);
    assert(errno == EPROTO && calls == before + 1);
    assert(gva_manager_register_aperture(42, &request, &observed) == -1);
    assert(errno == EPROTO);
    mode = APERTURE_GENERATION;
    assert(gva_manager_register_aperture(42, &request, &observed) == -1);
    mode = IO_ERROR;
    assert(gva_manager_register_aperture(42, &request, &observed) == -1);
    assert(errno == EBUSY);
    mode = VALID;
    mode = VALID;
    segment = descriptor();
    before = calls;
    assert(gva_manager_export_segment(42, &segment, &exported) == 0);
    assert(calls == before + 2);
    assert(exported.mem_id == 91 && exported.tokenid == 97);
    assert(segment.token_id == 11 && segment.token_value == 53);
    before = calls;
    assert(gva_manager_unexport_segment(42, &segment, &exported) == 0);
    assert(calls == before + 2);
    mode = STALE_TOKEN;
    before = calls;
    assert(gva_manager_unexport_segment(42, &segment, &exported) == -1);
    assert(errno == ESTALE && calls == before + 1);
    mode = UNEXPORT_ERROR;
    assert(gva_manager_unexport_segment(42, &segment, &exported) == -1);
    assert(errno == EBUSY && exported.mem_id == 91);
    mode = VALID;
    exported.uba++;
    before = calls;
    assert(gva_manager_unexport_segment(42, &segment, &exported) == -1);
    assert(errno == EINVAL && calls == before);
    exported.uba--;
    mode = STALE_TOKEN;
    before = calls;
    assert(gva_manager_export_segment(42, &segment, &exported) == -1);
    assert(errno == ESTALE && calls == before + 1 && exported.mem_id == 0);
    mode = EXPORT_ADDRESS;
    assert(gva_manager_export_segment(42, &segment, &exported) == -1);
    assert(errno == EPROTO && exported.mem_id == 91);
    mode = EXPORT_ERROR;
    assert(gva_manager_export_segment(42, &segment, &exported) == -1);
    assert(errno == EIO && exported.mem_id == 91);
    mode = VALID;
    before = calls;
    assert(gva_manager_export_segment(42, NULL, &exported) == -1);
    assert(gva_manager_export_segment(42, &segment, NULL) == -1);
    assert(calls == before);

    _Static_assert(sizeof(struct obmm_cmd_gsva_export_segment_v1) == 120,
                   "checked GSVA export has a fixed-width ABI");
    for (int fast = 0; fast <= 1; fast++) {
        mode = VALID;
        before = calls;
        assert(!gva_manager_export_segment_checked(42, &segment, fast != 0, &exported));
        assert(calls == before + 1 && exported.mem_id == 91 && exported.tokenid == 97);
        for (mode = CHECKED_EMPTY; mode <= CHECKED_COPY_LOST; mode++) {
            before = calls;
            int result = gva_manager_export_segment_checked(42, &segment, fast != 0, &exported);
            assert(calls == before + 1); /* Never retry the legacy opcode. */
            assert(result == (mode == CHECKED_EMPTY ? 1 : -1));
            if (mode == CHECKED_EMPTY || mode == CHECKED_UNKNOWN) assert(errno == ENOMEM);
            else if (mode == CHECKED_OLD_KERNEL) assert(errno == ENOTTY);
            else if (mode == CHECKED_COPY_LOST) assert(errno == EFAULT && exported.mem_id == 91);
            else assert(errno == EPROTO);
        }
    }
    mode = VALID;
    before = calls;
    request.node_id = request.node_count;
    assert(gva_manager_register_aperture(42, &request, &observed) == -1);
    assert(errno == EINVAL && calls == before);

    assert(gva_manager_allocate_segment(42, &alloc) == 0);
    assert(alloc.desc.token_id == 11 && alloc.desc.token_value == 53);
    assert(alloc.desc.epoch == 19);
    alloc.requested_home_va = 0;
    assert(!gva_manager_allocate_segment(42, &alloc));
    assert(alloc.desc.home_va == 0x700000000000ULL);
    for (mode = ALLOC_ENOSPC; mode <= ALLOC_OUTPUT_LOST; mode++) {
        struct obmm_gsva_segment_desc_v1 empty = {0};
        memset(&alloc.desc, 0, sizeof(alloc.desc));
        before = calls;
        assert(gva_manager_allocate_segment(42, &alloc) == -1);
        assert(errno == (mode == ALLOC_ENOSPC ? ENOSPC : EFAULT));
        assert(calls == before + 1 && !memcmp(&alloc.desc, &empty, sizeof(empty)));
    }
    alloc.requested_home_va = 0x700000000000ULL;
    for (i = 0; i < sizeof(bad_alloc) / sizeof(bad_alloc[0]); i++) {
        mode = bad_alloc[i];
        assert(gva_manager_allocate_segment(42, &alloc) == -1);
        assert(errno == EPROTO && alloc.desc.segment_id == 73);
    }
    mode = VALID;
    before = calls;
    alloc.alignment = 65537;
    assert(gva_manager_allocate_segment(42, &alloc) == -1);
    assert(errno == EINVAL && calls == before);
    alloc.alignment = 65536;
    alloc.requested_home_va = 0xffffffffffff0000ULL;
    assert(gva_manager_allocate_segment(42, &alloc) == -1);
    assert(errno == EINVAL && calls == before);
    alloc.requested_home_va = 0x700000000000ULL;
    mode = IO_ERROR;
    assert(gva_manager_allocate_segment(42, &alloc) == -1);
    assert(errno == EBUSY);

    mode = VALID;
    assert(gva_manager_query_segment(42, &query) == 0);
    mode = BAD_ID;
    assert(gva_manager_query_segment(42, &query) == -1);
    assert(errno == EPROTO);
    mode = RETIRED;
    assert(gva_manager_query_segment(42, &query) == -1);
    mode = VALID;
    query.segment_id = 0;
    query.home_va = 0x700000000000ULL;
    assert(gva_manager_query_segment(42, &query) == 0);
    query.home_va += 65536;
    assert(gva_manager_query_segment(42, &query) == -1);
    assert(errno == EPROTO);

    assert(gva_manager_retire_segment(42, &retire) == 0);
    for (i = 0; i < sizeof(bad_retire) / sizeof(bad_retire[0]); i++) {
        mode = bad_retire[i];
        assert(gva_manager_retire_segment(42, &retire) == -1);
        assert(errno == EPROTO);
    }
    before = calls;
    assert(gva_manager_allocate_segment(42, NULL) == -1);
    assert(gva_manager_query_segment(42, NULL) == -1);
    assert(gva_manager_retire_segment(42, NULL) == -1);
    assert(calls == before);
    puts("gva_manager_golden=pass");
    return 0;
}
