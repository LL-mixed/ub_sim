/* Executable worker transaction test; kernel and publish calls are wrapped. */
#include "components/mem_service/providers/mem_service_provider_obmm_worker.c"
#include <assert.h>
#include <stdarg.h>
#include <sys/wait.h>

enum failure_mode {
    GOOD, ALLOC_FAIL, EXPORT_FAIL, PUBLISH_FAIL, BAD_REPLY,
    UNEXPORT_FAIL, RETIRE_FAIL, RECLAIM_FAIL, BAD_RECLAIM_REPLY,
    CANCEL_FAIL, BAD_CANCEL_REPLY, BAD_CANCEL_BINDING, BACKED_CANCEL_REPLY,
    BACKING_EMPTY, ADDRESS_FULL, ADDRESS_FULL_WITH_DESC, ALLOC_COPY_LOST,
    ALLOC_EMPTY_IO_ERROR, ALLOC_OUTSIDE_APERTURE, ADDRESS_FULL_JOURNAL_FAIL,
    ADDRESS_BUSY_ONCE
};
static enum failure_mode mode;
static unsigned allocated, exports, publishes;
static unsigned unexports, retires, reclaims;
static bool publish_cancelled, empty_cancel;
static bool drive_worker_loop;
static bool drive_worker_recovery;
static bool drive_worker_resume;
static bool drive_worker_quarantine;
static bool drive_worker_holder_fence;
static bool drive_worker_terminated_holder_fence;
static bool drive_worker_holder_rejoin;
static bool holder_rejoin_home_pending;
static bool holder_rejoin_holder_pending;
static enum mem_service_wire_status quarantine_status = MEM_SERVICE_WIRE_STATUS_OK;
static unsigned provider_deregistrations;
static bool resume_service_mismatch;
static const char *resume_service_state = "active";
static bool resume_service_backed = true;
static struct mem_service_client_allocation resume_identity;
static struct obmm_gsva_segment_desc_v1 resume_service_segment;
static uint64_t resume_service_mem_id, resume_service_token_id;
static bool rollback_empty;
static unsigned cancellations, loop_polls;
static uint64_t kernel_address = 0x700000000000ULL;
static unsigned busy_rejections;
static bool fail_next_sync;
static const struct mem_service_client_allocation *direct_work;
static struct mem_service_client_allocation polled_work;
static struct obmm_cmd_gsva_enumerate_v1 kernel_record;
static unsigned inventory_mode, inventory_calls;
static unsigned recovery_polls, recovery_reclaims, recovery_inspects;
static unsigned recovery_home_receipts;
static unsigned holder_recovery_polls, holder_physical_revokes, holder_receipts;
static bool holder_physical_fail;

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_next_sync) { fail_next_sync = false; errno = EIO; return -1; }
    return __real_fsync(fd);
}

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t create_mode = 0;
    if ((drive_worker_loop || drive_worker_recovery || drive_worker_resume ||
         drive_worker_holder_fence || drive_worker_terminated_holder_fence) &&
        !strcmp(path, "/dev/obmm")) return 42;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        create_mode = va_arg(args, int);
        va_end(args);
    }
    return __real_open(path, flags, create_mode);
}

int __wrap_close(int fd)
{
    return (drive_worker_loop || drive_worker_recovery || drive_worker_resume ||
            drive_worker_holder_fence ||
            drive_worker_terminated_holder_fence) && fd == 42
               ? 0
               : __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long op, ...)
{
    va_list args;
    void *argument;
    assert((drive_worker_loop || drive_worker_recovery || drive_worker_resume ||
            drive_worker_holder_fence ||
            drive_worker_terminated_holder_fence) && fd == 42);
    va_start(args, op);
    argument = va_arg(args, void *);
    va_end(args);
    if (op == OBMM_CMD_GSVA_ENUMERATE) {
        struct obmm_cmd_gsva_enumerate_v1 *reply = argument;
        uint64_t previous = reply->cursor;
        inventory_calls++;
        if (inventory_mode == 8 || (inventory_mode == 6 && previous)) {
            errno = inventory_mode == 8 ? ENOTTY : ESTALE;
            return -1;
        }
        memset(reply, 0, sizeof(*reply));
        if (inventory_mode && inventory_mode != 3 && inventory_mode != 9 &&
            !previous) {
            *reply = kernel_record;
            reply->flags = OBMM_GSVA_ENUM_ENTRY;
            reply->cursor = reply->desc.segment_id & ((1ULL << 48) - 1);
            if (inventory_mode == 4) reply->export_token_id++;
            if (inventory_mode == 5) reply->resource_flags |= OBMM_GSVA_RESOURCE_EXPORT_BUSY;
            if (inventory_mode == 7) { reply->desc.segment_id++; reply->cursor++; }
        } else { reply->flags = OBMM_GSVA_ENUM_END; reply->cursor = previous; }
        reply->version = 1;
        reply->revision = 1;
        reply->kernel_instance[0] =
            inventory_mode == 2 || inventory_mode == 9 ? 2 : 1;
        return 0;
    }
    if (op == OBMM_CMD_GSVA_EVENT_V1) {
        struct obmm_cmd_gsva_event_v1 *event = argument;

        assert(drive_worker_holder_fence ||
               drive_worker_terminated_holder_fence);
        assert(event->version == OBMM_GSVA_ABI_VERSION &&
               event->sub_op == OBMM_GSVA_EVENT_LOCAL_REVOKE &&
               !event->requester_cna);
        assert(event->key.segment_id == kernel_record.desc.segment_id &&
               event->key.home_va == kernel_record.desc.home_va &&
               event->key.size == kernel_record.desc.size &&
               event->key.epoch == kernel_record.desc.epoch &&
               event->token_id == kernel_record.desc.token_id &&
               event->token_value == kernel_record.desc.token_value);
        holder_physical_revokes++;
        event->error = drive_worker_terminated_holder_fence
                           ? GSVA_ERR_ROUTE_MISSING
                           : holder_physical_fail ? GSVA_ERR_COH_PENDING
                                                  : GSVA_OK;
        return 0;
    }
    assert(op == OBMM_CMD_GSVA_APERTURE_QUERY);
    *(struct obmm_cmd_gsva_aperture *)argument = (struct obmm_cmd_gsva_aperture){.base = 0x700000000000ULL,
        .size = 0x1000000, .node_count = 2, .flags = OBMM_GSVA_APERTURE_F_ACTIVE};
    return 0;
}

int __wrap_mem_service_client_provider_refresh(const struct mem_service_client *client,
    const char *node, uint64_t incarnation, uint64_t readiness_generation,
    struct mem_service_client_provider_directory *directory, enum mem_service_wire_status *status)
{
    (void)client;
    assert((drive_worker_loop || drive_worker_recovery || drive_worker_resume ||
            drive_worker_holder_fence || drive_worker_terminated_holder_fence ||
            drive_worker_holder_rejoin) &&
           !strcmp(node, "nodeA"));
    assert(incarnation ==
           ((drive_worker_recovery || drive_worker_holder_fence ||
             drive_worker_terminated_holder_fence ||
             drive_worker_holder_rejoin) ? 8 : 7));
    assert(readiness_generation ==
           ((drive_worker_recovery || drive_worker_holder_fence ||
             drive_worker_terminated_holder_fence ||
             drive_worker_holder_rejoin) ? 2 : 1));
    memset(directory, 0, sizeof(*directory));
    /* The real refresh reply omits data_plane_ready; it stays false. */
    directory->directory_ready = true;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_poll_allocation(const struct mem_service_client *client,
    const char *node, uint64_t incarnation, uint64_t after_generation,
    struct mem_service_client_allocation *work, enum mem_service_wire_status *status)
{
    (void)client;
    assert((drive_worker_loop || drive_worker_resume) && !strcmp(node, "nodeA") &&
           incarnation == 7 && !after_generation);
    if (drive_worker_resume) {
        worker_stop = 1;
        *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
        return 0;
    }
    *work = (struct mem_service_client_allocation){.key = "object", .state = "allocating",
        .home_node = "nodeA", .provider_incarnation = 7, .generation = 99,
        .size_bytes = 17, .capabilities = 1};
    if (loop_polls++ == 0) {
        work->generation = 98;
        work->size_bytes = UINT64_MAX;
    }
    polled_work = *work;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_poll_recovery_allocation(
    const struct mem_service_client *client, const char *node,
    uint64_t current_incarnation, uint64_t fenced_incarnation,
    uint64_t after_generation, struct mem_service_client_allocation *work,
    enum mem_service_wire_status *status)
{
    (void)client;
    assert((drive_worker_recovery || drive_worker_holder_rejoin) &&
           !strcmp(node, "nodeA"));
    assert(current_incarnation == 8 && fenced_incarnation == 7);
    if (drive_worker_holder_rejoin) {
        assert(!after_generation);
        if (holder_rejoin_home_pending) {
            memset(work, 0, sizeof(*work));
            strcpy(work->key, "unexpected-home");
            strcpy(work->state, "quarantined");
            work->generation = 101;
            *status = MEM_SERVICE_WIRE_STATUS_OK;
        } else {
            *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
        }
        return 0;
    }
    if (!recovery_reclaims && !recovery_polls++) {
        memset(work, 0, sizeof(*work));
        strcpy(work->key, "object");
        strcpy(work->state, "quarantined");
        strcpy(work->home_node, "nodeA");
        work->provider_incarnation = 7;
        work->generation = 99;
        work->size_bytes = 17;
        work->capabilities = 1;
        work->holder_count = 1;
        strcpy(work->holders[0].session_id, "home-session");
        strcpy(work->holders[0].node_id, "nodeA");
        work->holders[0].generation = work->generation;
        work->holders[0].provider_incarnation = 7;
        assert(after_generation == 0);
        *status = MEM_SERVICE_WIRE_STATUS_OK;
        return 0;
    }
    assert(after_generation == (recovery_polls ? 99 : 0));
    *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
    return 0;
}

int __wrap_mem_service_client_poll_holder_recovery_allocation(
    const struct mem_service_client *client, const char *node,
    uint64_t current_incarnation, uint64_t fenced_incarnation,
    uint64_t after_generation, struct mem_service_client_allocation *work,
    enum mem_service_wire_status *status)
{
    struct obmm_cmd_export exported = {0};
    struct mem_service_provider_descriptor descriptor;

    (void)client;
    assert((drive_worker_holder_fence || drive_worker_terminated_holder_fence ||
            drive_worker_holder_rejoin) &&
           !strcmp(node, "nodeA") &&
           current_incarnation == 8 && fenced_incarnation == 7);
    if (drive_worker_holder_rejoin) {
        assert(!after_generation);
        if (holder_rejoin_holder_pending) {
            memset(work, 0, sizeof(*work));
            strcpy(work->key, "unexpected-holder");
            strcpy(work->state, "quarantined");
            work->generation = 102;
            *status = MEM_SERVICE_WIRE_STATUS_OK;
        } else {
            *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
        }
        return 0;
    }
    if (holder_receipts || holder_recovery_polls++) {
        assert(after_generation == 99);
        *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
        return 0;
    }
    assert(!after_generation);
    exported.length = 1;
    exported.size[0] = kernel_record.desc.size;
    exported.flags = OBMM_EXPORT_FLAG_GSVA_FIXED_UBA |
                     OBMM_EXPORT_FLAG_ALLOW_MMAP;
    exported.uba = kernel_record.desc.home_va;
    exported.mem_id = kernel_record.export_mem_id;
    exported.tokenid = kernel_record.export_token_id;
    assert(!mem_service_provider_obmm_encode_gsva(
        &kernel_record.desc, &exported, &descriptor));
    memset(work, 0, sizeof(*work));
    strcpy(work->key, "remote-object");
    strcpy(work->state, "quarantined");
    strcpy(work->home_node, "nodeB");
    work->provider_incarnation = 3;
    work->generation = 99;
    work->size_bytes = kernel_record.desc.size;
    work->capabilities = MEM_SERVICE_MANAGED_CAP_MAP;
    work->provider_backed = true;
    work->address = kernel_record.desc.home_va;
    work->address_len = kernel_record.desc.size;
    work->descriptor_len = descriptor.len;
    memcpy(work->descriptor, descriptor.bytes, descriptor.len);
    work->holder_count = 1;
    strcpy(work->holders[0].session_id, "remote-session");
    strcpy(work->holders[0].node_id, "nodeA");
    work->holders[0].generation = work->generation;
    work->holders[0].provider_incarnation = 7;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_fence_allocation_holder(
    const struct mem_service_client *client, const char *key,
    const char *holder_node_id, uint64_t current_incarnation,
    uint64_t generation, uint64_t fenced_incarnation,
    const char *idempotency_key,
    struct mem_service_client_allocation *out,
    enum mem_service_wire_status *status)
{
    (void)client;
    if (drive_worker_holder_fence || drive_worker_terminated_holder_fence) {
        assert(holder_physical_revokes && !holder_physical_fail &&
               !strcmp(key, "remote-object") &&
               !strcmp(holder_node_id, "nodeA") &&
               current_incarnation == 8 && generation == 99 &&
               fenced_incarnation == 7 &&
               !strcmp(idempotency_key,
                       drive_worker_terminated_holder_fence
                           ? "obmm-holder-reboot-0000000000000007-0000000000000063"
                           : "obmm-holder-fence-0000000000000007-0000000000000063"));
        holder_receipts++;
    } else {
        assert(drive_worker_recovery && !strcmp(key, "object") &&
               !strcmp(holder_node_id, "nodeA") &&
               current_incarnation == 8 && generation == 99 &&
               fenced_incarnation == 7 &&
               !strcmp(idempotency_key,
                       "obmm-home-absence-0000000000000007-0000000000000063"));
        recovery_home_receipts++;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->key, key);
    strcpy(out->state, "quarantined");
    out->generation = generation;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_recover_allocation(
    const struct mem_service_client *client, const char *key, const char *node,
    uint64_t current_incarnation, uint64_t generation,
    uint64_t fenced_incarnation, bool backing_gone,
    struct mem_service_client_allocation *out,
    enum mem_service_wire_status *status)
{
    (void)client;
    assert(drive_worker_recovery && !strcmp(key, "object") &&
           !strcmp(node, "nodeA") && current_incarnation == 8 &&
           generation == 99 && fenced_incarnation == 7 && backing_gone &&
           recovery_home_receipts == 1);
    recovery_reclaims++;
    memset(out, 0, sizeof(*out));
    strcpy(out->key, key);
    strcpy(out->state, "retired");
    strcpy(out->home_node, node);
    out->provider_incarnation = fenced_incarnation;
    out->generation = generation;
    out->size_bytes = 17;
    out->capabilities = 1;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_inspect_allocation(
    const struct mem_service_client *client, const char *key,
    struct mem_service_client_allocation *out,
    enum mem_service_wire_status *status)
{
    (void)client;
    assert((drive_worker_recovery || drive_worker_resume) && !strcmp(key, "object"));
    if (drive_worker_resume) {
        struct obmm_cmd_export exported = {0};
        struct mem_service_provider_descriptor descriptor;

        memset(out, 0, sizeof(*out));
        *out = resume_identity;
        strcpy(out->key, key);
        strcpy(out->state, resume_service_state);
        if (!out->home_node[0]) strcpy(out->home_node, "nodeA");
        if (!out->provider_incarnation) out->provider_incarnation = 7;
        if (!out->generation) out->generation = 99;
        if (!out->size_bytes) out->size_bytes = 17;
        if (!out->capabilities) out->capabilities = 1;
        out->provider_backed = resume_service_backed;
        if (resume_service_backed) {
            if (!resume_service_segment.segment_id) {
                resume_service_segment = kernel_record.desc;
                resume_service_mem_id = kernel_record.export_mem_id;
                resume_service_token_id = kernel_record.export_token_id;
            }
            exported.length = 1;
            exported.flags = OBMM_EXPORT_FLAG_GSVA_FIXED_UBA |
                             OBMM_EXPORT_FLAG_ALLOW_MMAP;
            exported.uba = resume_service_segment.home_va;
            exported.size[0] = resume_service_segment.size;
            exported.mem_id = resume_service_mem_id;
            exported.tokenid = resume_service_token_id;
            assert(!mem_service_provider_obmm_encode_gsva(
                &resume_service_segment, &exported, &descriptor));
            out->address = resume_service_segment.home_va + resume_service_mismatch;
            out->address_len = resume_service_segment.size;
            out->descriptor_len = descriptor.len;
            memcpy(out->descriptor, descriptor.bytes, descriptor.len);
        } else {
            out->address = out->address_len = out->descriptor_len = 0;
            memset(out->descriptor, 0, sizeof(out->descriptor));
        }
        *status = MEM_SERVICE_WIRE_STATUS_OK;
        return 0;
    }
    recovery_inspects++;
    memset(out, 0, sizeof(*out));
    strcpy(out->key, key);
    strcpy(out->state, "retired");
    strcpy(out->home_node, "nodeA");
    out->provider_incarnation = 7;
    out->generation = 99;
    out->size_bytes = 17;
    out->capabilities = 1;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_provider_deregister(const struct mem_service_client *client,
    const char *node, uint64_t incarnation, struct mem_service_client_provider_directory *directory,
    enum mem_service_wire_status *status)
{
    (void)client;
    assert((drive_worker_loop || drive_worker_resume || drive_worker_quarantine) &&
           !strcmp(node, "nodeA") && incarnation == 7);
    memset(directory, 0, sizeof(*directory));
    provider_deregistrations++;
    *status = drive_worker_quarantine ? quarantine_status :
                                        MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_gva_manager_allocate_segment(int fd, struct obmm_cmd_gsva_alloc_segment_v1 *cmd)
{
    assert(fd == 42 && cmd->requested_home_va != 0);
    allocated++;
    if (mode == ADDRESS_BUSY_ONCE && busy_rejections == 0) {
        busy_rejections++;
        errno = EBUSY;
        return -1;
    }
    if (mode == ADDRESS_FULL || mode == ADDRESS_FULL_JOURNAL_FAIL ||
        mode == ALLOC_COPY_LOST || mode == ALLOC_EMPTY_IO_ERROR) {
        fail_next_sync = mode == ADDRESS_FULL_JOURNAL_FAIL;
        errno = (mode == ADDRESS_FULL || fail_next_sync) ? ENOSPC :
                mode == ALLOC_COPY_LOST ? EFAULT : EIO;
        return -1;
    }
    cmd->desc = (struct obmm_gsva_segment_desc_v1){
        .version = 1, .flags = 7, .segment_id = 9 + allocated,
        .home_va = cmd->requested_home_va,
        .size = cmd->size, .epoch = 1, .home_cna = 7, .owner_node_id = 0,
        .node_count = 2, .cache_policy = cmd->cache_policy, .access_flags = 3,
        .token_id = 2 + allocated, .token_value = 3 + allocated,
    };
    if (mode == ADDRESS_FULL_WITH_DESC) { errno = ENOSPC; return -1; }
    if (mode == ALLOC_OUTSIDE_APERTURE) cmd->desc.home_va += 0x10000000;
    if (mode == ALLOC_FAIL) errno = EIO;
    return mode == ALLOC_FAIL ? -1 : 0;
}

int __wrap_gva_manager_export_segment_checked(int fd,
    const struct obmm_gsva_segment_desc_v1 *segment, bool fast, struct obmm_cmd_export *out)
{
    (void)fast;
    assert(fd == 42);
    exports++;
    memset(out, 0, sizeof(*out));
    if (mode == BACKING_EMPTY) {
        errno = ENOMEM;
        return 1;
    }
    out->length = 1;
    out->size[0] = segment->size;
    out->uba = segment->home_va;
    out->flags = OBMM_EXPORT_FLAG_GSVA_FIXED_UBA | OBMM_EXPORT_FLAG_ALLOW_MMAP;
    out->mem_id = 17;
    out->tokenid = 97;
    if (mode == EXPORT_FAIL) errno = ENOMEM; /* errno alone is not a receipt. */
    if (drive_worker_resume && mode != EXPORT_FAIL) {
        kernel_record.resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED;
        kernel_record.export_mem_id = out->mem_id;
        kernel_record.export_token_id = out->tokenid;
    }
    return mode == EXPORT_FAIL ? -1 : 0;
}

int __wrap_mem_service_client_publish_allocation(
    const struct mem_service_client *client, const char *key, const char *node,
    uint64_t incarnation, uint64_t generation, const uint8_t *descriptor,
    uint32_t descriptor_len, uint64_t address, uint64_t address_len,
    struct mem_service_client_allocation *out, enum mem_service_wire_status *status)
{
    (void)client;
    assert(!strcmp(key, "object") && !strcmp(node, "nodeA") && incarnation == 7);
    assert(descriptor_len == 96);
    publishes++;
    if (mode == PUBLISH_FAIL) {
        *status = MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT;
        return 1;
    }
    memset(out, 0, sizeof(*out));
    strcpy(out->state, publish_cancelled ? "retiring" : "active");
    out->generation = generation;
    out->address = address + (mode == BAD_REPLY);
    out->address_len = address_len;
    out->descriptor_len = descriptor_len;
    memcpy(out->descriptor, descriptor, descriptor_len);
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    if (drive_worker_resume) {
        resume_service_state = publish_cancelled ? "retiring" : "active";
        resume_service_backed = true;
        resume_service_segment = kernel_record.desc;
        resume_service_mem_id = kernel_record.export_mem_id;
        resume_service_token_id = kernel_record.export_token_id;
    }
    if (drive_worker_loop) worker_stop = 1;
    return 0;
}

int __wrap_gva_manager_unexport_segment(int fd,
    const struct obmm_gsva_segment_desc_v1 *segment, const struct obmm_cmd_export *exported)
{
    assert(fd == 42 &&
           segment->segment_id == (drive_worker_resume ?
                                      kernel_record.desc.segment_id :
                                      9 + allocated) &&
           exported->mem_id == 17);
    unexports++;
    if (drive_worker_resume && mode != UNEXPORT_FAIL) {
        kernel_record.export_mem_id = 0;
        kernel_record.export_token_id = 0;
    }
    return mode == UNEXPORT_FAIL ? -1 : 0;
}

int __wrap_gva_manager_retire_segment(int fd, struct obmm_cmd_gsva_retire_segment_v1 *retire)
{
    assert(fd == 42 &&
           retire->segment_id == (drive_worker_resume ?
                                     kernel_record.desc.segment_id :
                                     9 + allocated) &&
           retire->epoch == 1);
    if (!drive_worker_resume)
        assert(rollback_empty ? unexports == 0 : unexports == retires + 1);
    retires++;
    if (drive_worker_resume && mode != RETIRE_FAIL) {
        kernel_record.desc.flags =
            (kernel_record.desc.flags & ~OBMM_GSVA_SEG_F_ACTIVE) |
            OBMM_GSVA_SEG_F_RETIRED;
        kernel_record.resource_flags = 0;
        kernel_record.export_mem_id = 0;
        kernel_record.export_token_id = 0;
    }
    return mode == RETIRE_FAIL ? -1 : 0;
}

int __wrap_mem_service_client_reclaim_allocation(
    const struct mem_service_client *client, const char *key, const char *node,
    uint64_t incarnation, uint64_t generation, bool confirmed,
    struct mem_service_client_allocation *out, enum mem_service_wire_status *status)
{
    (void)client;
    assert(!strcmp(key, "object") && !strcmp(node, "nodeA") && incarnation == 7);
    assert(confirmed && (drive_worker_resume || empty_cancel ||
                         retires == reclaims + 1));
    reclaims++;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    memset(out, 0, sizeof(*out));
    strcpy(out->state, "retired");
    out->generation = generation + (mode == BAD_RECLAIM_REPLY);
    if (drive_worker_resume && mode != RECLAIM_FAIL &&
        mode != BAD_RECLAIM_REPLY) {
        resume_service_state = "retired";
        resume_service_backed = false;
    }
    return mode == RECLAIM_FAIL ? -1 : 0;
}

int __wrap_mem_service_client_retire_object(
    const struct mem_service_client *client, const char *key, const char *operation,
    bool has_generation, uint64_t generation, struct mem_service_client_allocation *out,
    enum mem_service_wire_status *status)
{
    (void)client;
    char expected[96];
    snprintf(expected, sizeof(expected), "obmm-capacity-7-%" PRIu64, generation);
    assert(!strcmp(key, "object") && !strcmp(operation, expected) && has_generation);
    cancellations++;
    *out = drive_worker_resume ? resume_identity :
           drive_worker_loop ? polled_work : *direct_work;
    if (drive_worker_resume) {
        strcpy(out->key, key);
        strcpy(out->home_node, "nodeA");
        out->provider_incarnation = 7;
    }
    strcpy(out->state, "retiring");
    out->generation = generation + (mode == BAD_CANCEL_REPLY);
    if (mode == BAD_CANCEL_BINDING) out->provider_incarnation++;
    if (mode == BACKED_CANCEL_REPLY) out->provider_backed = true;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    if (drive_worker_resume && mode != CANCEL_FAIL &&
        mode != BAD_CANCEL_REPLY && mode != BAD_CANCEL_BINDING &&
        mode != BACKED_CANCEL_REPLY)
        resume_service_state = "retiring";
    return mode == CANCEL_FAIL ? -1 : 0;
}

static void run_worker_fault_injection_case(const char *fault_mode, bool torn)
{
    char directory[] = "/tmp/gva-worker-fault-XXXXXX";
    char config_path[256], state_path[256];
    struct worker_config saved;
    struct worker_ledger_entry entry;
    struct stat state;
    unsigned deregistrations_before;
    int fd, status;
    pid_t child;
    FILE *file;

    assert(mkdtemp(directory));
    snprintf(config_path, sizeof(config_path), "%s/worker.conf", directory);
    snprintf(state_path, sizeof(state_path), "%s/worker.state", directory);
    file = fopen(config_path, "w");
    assert(file);
    fprintf(file, "connect=unix:/unused\nnode_id=nodeA\nincarnation=7\n"
            "readiness_generation=1\nallocation_granularity_bytes=2097152\n"
            "state_file=%s\nfault_injection_phase=reserve-intent\n"
            "fault_injection_mode=%s\n", state_path, fault_mode);
    assert(!fclose(file));

    drive_worker_loop = true;
    drive_worker_quarantine = false;
    empty_cancel = true;
    inventory_mode = loop_polls = 0;
    mode = GOOD;
    child = fork();
    assert(child >= 0);
    if (!child) {
        int result = mem_service_provider_obmm_serve_allocations(config_path);
        _exit(result);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == WORKER_FAULT_INJECTION_EXIT_CODE);
    drive_worker_loop = false;
    assert(!stat(state_path, &state));
    assert((size_t)state.st_size % WORKER_LEDGER_FRAME_BYTES ==
           (torn ? WORKER_LEDGER_FRAME_BYTES / 2 : 0));
    assert(!read_config(config_path, &saved));
    fd = open(state_path, O_RDONLY);
    assert(fd >= 0);
    if (!torn) {
        uint64_t sequence = (uint64_t)state.st_size / WORKER_LEDGER_FRAME_BYTES;
        assert(sequence >= 2 && !ledger_read_entry(fd, 1, &saved, &entry) &&
               lseek(fd, (off_t)(sequence - 1) * WORKER_LEDGER_FRAME_BYTES,
                     SEEK_SET) >= 0 &&
               !ledger_read_entry(fd, sequence, &saved, &entry));
        assert(!strcmp(entry.phase, "reserve-intent") && entry.work.generation);
    }
    assert(!close(fd));

    drive_worker_quarantine = true;
    quarantine_status = MEM_SERVICE_WIRE_STATUS_OK;
    deregistrations_before = provider_deregistrations;
    assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
    assert(provider_deregistrations == deregistrations_before + 1);
    drive_worker_quarantine = false;
    assert(!unlink(state_path) && !unlink(config_path) && !rmdir(directory));
}

int main(void)
{
    struct worker_config config = {.node = "nodeA", .incarnation = 7,
                                  .allocation_granularity_bytes = 2097152,
                                  .kernel_instance = {1}};
    struct mem_service_client client = {0};
    struct obmm_cmd_gsva_aperture aperture = {.base = 0x700000000000ULL, .size = 0x10000000};
    struct mem_service_client_allocation work = {
        .key = "object", .state = "allocating", .home_node = "nodeA",
        .generation = 1, .size_bytes = 65536, .alignment_bytes = 65536,
        .capabilities = 1, .provider_incarnation = 7,
    };
    FILE *journal = tmpfile();
    struct worker_reservation reservation = {0};
    direct_work = &work;
    assert(journal);
    for (mode = GOOD; mode <= BAD_REPLY; mode++) {
        unsigned old_alloc = allocated, old_export = exports, old_publish = publishes;
        memset(&reservation, 0, sizeof(reservation));
        int rc = allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                               &reservation);
        assert((rc == 0) == (mode == GOOD));
        assert(allocated == old_alloc + 1);
        assert(exports == old_export + (mode != ALLOC_FAIL));
        assert(publishes == old_publish + (mode != ALLOC_FAIL && mode != EXPORT_FAIL));
    }
    mode = GOOD;
    {
        unsigned before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        assert(allocate_work(42, -1, &config, &client, &aperture, &work,
                             &reservation) != 0);
        strcpy(work.state, "retiring");
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) != 0);
        assert(allocated == before);
    }
    for (int failure = GOOD; failure <= BAD_RECLAIM_REPLY; failure++) {
        struct mem_service_client_allocation release = {0};
        mode = GOOD;
        memset(&reservation, 0, sizeof(reservation));
        strcpy(work.state, "allocating");
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) == 0);
        assert(reservation.segment.home_va == kernel_address);
        assert(reservation.segment.size == config.allocation_granularity_bytes);
        release = work;
        strcpy(release.state, "retiring");
        release.provider_backed = true;
        release.address = reservation.segment.home_va;
        release.address_len = reservation.segment.size;
        release.descriptor_len = reservation.descriptor.len;
        memcpy(release.descriptor, reservation.descriptor.bytes, release.descriptor_len);
        unexports = retires = reclaims = 0;
        release.live_refs = 1;
        assert(reclaim_work(42, fileno(journal), &config, &client, &release, &reservation));
        release.live_refs = 0;
        release.holder_count = 1;
        assert(reclaim_work(42, fileno(journal), &config, &client, &release, &reservation));
        release.holder_count = 0;
        release.descriptor[0] ^= 1;
        assert(reclaim_work(42, fileno(journal), &config, &client, &release, &reservation));
        release.descriptor[0] ^= 1;
        release.generation++;
        assert(reclaim_work(42, fileno(journal), &config, &client, &release, &reservation));
        release.generation--;
        assert(unexports == 0 && retires == 0 && reclaims == 0);
        assert(reclaim_work(42, -1, &config, &client, &release, &reservation));
        assert(unexports == 0);
        mode = failure;
        int rc = reclaim_work(42, fileno(journal), &config, &client, &release, &reservation);
        bool failed = failure >= UNEXPORT_FAIL;
        assert((rc != 0) == failed && reservation.occupied == failed);
        assert(unexports == 1);
        assert(retires == (failure != UNEXPORT_FAIL));
        assert(reclaims == (failure != UNEXPORT_FAIL && failure != RETIRE_FAIL));
        if (!failed) {
            assert(reclaim_work(42, fileno(journal), &config, &client, &release, &reservation));
            assert(unexports == 1);
        }
    }
    {
        unsigned before = allocated;
        mode = GOOD;
        memset(&reservation, 0, sizeof(reservation));
        work.size_bytes = UINT64_MAX;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) == 1);
        assert(!reservation.occupied);
        assert(allocated == before);
        work.size_bytes = aperture.size + 1;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) == 1);
        assert(!reservation.occupied);
        assert(allocated == before);
        work.size_bytes = 2097153;
        assert(!allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                              &reservation));
        assert(allocated == before + 1 && reservation.segment.size == 4194304);
    }
    {
        /* Empty cancellation must remain generation-bound and must not
         * accept a reply containing a concurrently published reservation. */
        unsigned before = allocated, old_exports = exports, old_publishes = publishes;
        empty_cancel = true;
        mode = GOOD;
        unsigned old_cancel = cancellations, old_reclaim = reclaims;
        assert(reject_capacity_work(-1, &config, &client, &work));
        assert(cancellations == old_cancel && reclaims == old_reclaim);
        for (int failure = GOOD; failure <= BACKED_CANCEL_REPLY; failure++) {
            mode = failure;
            old_cancel = cancellations;
            old_reclaim = reclaims;
            bool failed = failure == RECLAIM_FAIL || failure == BAD_RECLAIM_REPLY ||
                          failure >= CANCEL_FAIL;
            assert((reject_capacity_work(fileno(journal), &config, &client, &work) != 0) == failed);
            assert(cancellations == old_cancel + 1);
            assert(reclaims == old_reclaim + (failure < CANCEL_FAIL));
        }
        assert(allocated == before && exports == old_exports && publishes == old_publishes);
        mode = GOOD;
        empty_cancel = false;
    }
    {
        const uint64_t sizes[] = {1, 4096, 65537, 2097152, 2097153};
        const uint64_t alignments[] = {0, 4096, 65536, 2097152, 8388608};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            for (size_t j = 0; j < sizeof(alignments) / sizeof(alignments[0]); j++) {
                uint64_t alignment = alignments[j] < 2097152 ? 2097152 : alignments[j];
                memset(&reservation, 0, sizeof(reservation));
                work.size_bytes = sizes[i];
                work.alignment_bytes = alignments[j];
                assert(!allocate_work(42, fileno(journal), &config, &client, &aperture,
                                      &work, &reservation));
                assert(reservation.segment.home_va == kernel_address);
                assert(!(reservation.segment.home_va & (alignment - 1)));
                assert(reservation.segment.size == ((sizes[i] + 2097151) & ~2097151ULL));
            }
        }
        unsigned before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        work.alignment_bytes = 3;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) == WORKER_ALLOCATE_ERROR);
        work.alignment_bytes = 1ULL << 63;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation) == WORKER_ALLOCATE_CAPACITY);
        assert(allocated == before && !reservation.occupied);
        work.alignment_bytes = 65536;
    }
    {
        unsigned before;

        work.size_bytes = 65536;
        mode = GOOD;
        aperture.node_id = 1;
        memset(&reservation, 0, sizeof(reservation));
        assert(!allocate_work(42, fileno(journal), &config, &client,
                              &aperture, &work, &reservation));
        assert(reservation.segment.home_va ==
               aperture.base + aperture.size / aperture.node_count);

        aperture.node_id = 0;
        mode = ADDRESS_BUSY_ONCE;
        busy_rejections = 0;
        before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        assert(!allocate_work(42, fileno(journal), &config, &client,
                              &aperture, &work, &reservation));
        assert(busy_rejections == 1 && allocated == before + 2);
        assert(reservation.segment.home_va ==
               aperture.base + config.allocation_granularity_bytes);
        mode = GOOD;
    }
    {
        unsigned before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        work.size_bytes = 17;
        publish_cancelled = true;
        assert(!allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                              &reservation));
        assert(allocated == before + 1 && reservation.occupied);
        publish_cancelled = false;
        strcpy(work.state, "retiring");
        empty_cancel = true;
        assert(cancel_unreserved_work(-1, &config, &client, &work));
        work.live_refs = 1;
        assert(cancel_unreserved_work(fileno(journal), &config, &client, &work));
        work.live_refs = 0;
        work.provider_backed = true;
        assert(cancel_unreserved_work(fileno(journal), &config, &client, &work));
        work.provider_backed = false;
        assert(!cancel_unreserved_work(fileno(journal), &config, &client, &work));
        assert(allocated == before + 1);
    }
    {
        const enum failure_mode failures[] = {GOOD, RETIRE_FAIL, CANCEL_FAIL,
            BAD_CANCEL_REPLY, BAD_CANCEL_BINDING, BACKED_CANCEL_REPLY,
            RECLAIM_FAIL, BAD_RECLAIM_REPLY};
        rollback_empty = empty_cancel = true;
        for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            unsigned before_publish = publishes;
            memset(&reservation, 0, sizeof(reservation));
            strcpy(work.state, "allocating");
            mode = BACKING_EMPTY;
            assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                                 &reservation) == WORKER_ALLOCATE_BACKING_EMPTY);
            assert(reservation.occupied && reservation.export_no_backing);
            assert(reservation.segment.home_va == kernel_address && publishes == before_publish);
            unexports = retires = reclaims = 0;
            reservation.exported.mem_id = 17;
            assert(rollback_unbacked_work(42, fileno(journal), &config, &client, &work,
                                          &reservation));
            reservation.exported.mem_id = 0;
            work.provider_incarnation++;
            assert(rollback_unbacked_work(42, fileno(journal), &config, &client, &work,
                                          &reservation));
            work.provider_incarnation--;
            assert(rollback_unbacked_work(42, -1, &config, &client, &work, &reservation));
            assert(retires == 0 && reclaims == 0);
            mode = failures[i];
            int rc = rollback_unbacked_work(42, fileno(journal), &config, &client, &work,
                                           &reservation);
            assert((rc == 0) == (mode == GOOD));
            assert(reservation.occupied == (mode != GOOD));
            assert(retires == 1 && unexports == 0 && publishes == before_publish);
        }
        rollback_empty = false;
    }
    {
        const enum failure_mode failures[] = {ADDRESS_FULL, ADDRESS_FULL_WITH_DESC,
            ALLOC_COPY_LOST, ALLOC_EMPTY_IO_ERROR, ALLOC_OUTSIDE_APERTURE,
            ADDRESS_FULL_JOURNAL_FAIL};
        strcpy(work.state, "allocating");
        work.size_bytes = 65536;
        for (size_t i = 0; i < sizeof(failures) / sizeof(failures[0]); i++) {
            unsigned old_exports = exports, old_publishes = publishes;
            mode = failures[i];
            memset(&reservation, 0, sizeof(reservation));
            int result = allocate_work(42, fileno(journal), &config, &client,
                                       &aperture, &work, &reservation);
            assert(result == (mode == ADDRESS_FULL ? WORKER_ALLOCATE_CAPACITY :
                                                     WORKER_ALLOCATE_ERROR));
            assert(reservation.occupied == (mode != ADDRESS_FULL));
            assert(exports == old_exports && publishes == old_publishes);
            if (reservation.occupied) {
                assert(reservation.generation == work.generation);
                assert(!strcmp(reservation.key, work.key));
            }
        }
    }
    {
        /* The generation rotates the first candidate within the local node
         * slice. The kernel remains authoritative for reuse, while stale
         * cleanup stays identity-bound. */
        struct mem_service_client_allocation previous = {0};
        struct obmm_gsva_segment_desc_v1 old_segment = {0};
        mode = GOOD;
        empty_cancel = false;
        for (unsigned cycle = 0; cycle < 100; cycle++) {
            struct mem_service_client_allocation release;
            unsigned before;
            memset(&reservation, 0, sizeof(reservation));
            work.generation++;
            work.size_bytes = 65536;
            assert(!allocate_work(42, fileno(journal), &config, &client,
                                  &aperture, &work, &reservation));
            assert(reservation.segment.home_va ==
                   aperture.base +
                       ((work.generation - 1) %
                        (aperture.size / aperture.node_count /
                         config.allocation_granularity_bytes)) *
                           config.allocation_granularity_bytes);
            assert(reservation.segment.segment_id != old_segment.segment_id);
            assert(reservation.segment.token_id != old_segment.token_id);
            assert(reservation.segment.token_value != old_segment.token_value);
            before = allocated;
            assert(allocate_work(42, fileno(journal), &config, &client,
                                 &aperture, &work, &reservation));
            assert(allocated == before); /* A live local slot cannot be replaced. */
            unexports = retires = reclaims = 0;
            if (cycle) {
                assert(reclaim_work(42, fileno(journal), &config, &client,
                                    &previous, &reservation));
                assert(!unexports && !retires && !reclaims);
            }
            release = work;
            strcpy(release.state, "retiring");
            release.provider_backed = true;
            release.address = reservation.segment.home_va;
            release.address_len = reservation.segment.size;
            release.descriptor_len = reservation.descriptor.len;
            memcpy(release.descriptor, reservation.descriptor.bytes, release.descriptor_len);
            old_segment = reservation.segment;
            assert(!reclaim_work(42, fileno(journal), &config, &client,
                                 &release, &reservation));
            assert(!reservation.occupied && unexports == 1 && retires == 1 && reclaims == 1);
            previous = release;
        }
    }
    {
        struct worker_config resume_config = {
            .node = "nodeA",
            .incarnation = 7,
            .allocation_granularity_bytes = 2097152,
            .kernel_instance = {1},
        };
        struct worker_resume_object object = {0};
        struct worker_reservation rebuilt = {0}, restored = {0};
        bool did_restore = false;
        FILE *resume_journal = tmpfile();

        assert(resume_journal);
        resume_identity = (struct mem_service_client_allocation){
            .key = "object", .state = "allocating", .home_node = "nodeA",
            .provider_incarnation = 7, .generation = 99,
            .size_bytes = 17, .capabilities = 1,
        };
        object.last.work = resume_identity;
        object.saved.occupied = true;
        strcpy(object.saved.key, "object");
        object.saved.generation = 99;
        object.saved.segment = (struct obmm_gsva_segment_desc_v1){
            .version = 1,
            .flags = OBMM_GSVA_SEG_F_STRICT_ADDRESS_IDENTITY |
                     OBMM_GSVA_SEG_F_TOKEN_VALUE_REQUIRED |
                     OBMM_GSVA_SEG_F_ACTIVE,
            .segment_id = 409, .home_va = kernel_address,
            .size = 2097152, .epoch = 1, .home_cna = 7,
            .owner_node_id = 0, .node_count = 2,
            .cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH,
            .access_flags = OBMM_GSVA_ACCESS_READ |
                            OBMM_GSVA_ACCESS_WRITE,
            .token_id = 41, .token_value = 42,
        };
        kernel_record = (struct obmm_cmd_gsva_enumerate_v1){
            .desc = object.saved.segment,
            .resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED,
        };
        assert(!strcmp(worker_resume_resource_match(
            &resume_config, &object.saved, &kernel_record, &rebuilt),
            "reserved-no-export"));
        kernel_record.export_mem_id = 17;
        kernel_record.export_token_id = 97;
        assert(!strcmp(worker_resume_resource_match(
            &resume_config, &object.saved, &kernel_record, &rebuilt),
            "export-bound"));
        assert(rebuilt.descriptor.len == 96 && rebuilt.exported.mem_id == 17);
        assert(worker_resume_action_for(
                   "reserve-intent", false, NULL) == WORKER_RESUME_INVALID);
        assert(worker_resume_action_for(
                   "reserved", true, "reserved-no-export") ==
               WORKER_RESUME_EXPORT_AND_PUBLISH);
        assert(worker_resume_action_for(
                   "export-unknown", true, "export-bound") ==
               WORKER_RESUME_PUBLISH);
        assert(worker_resume_action_for(
                   "release-intent", true, "reserved-no-export") ==
               WORKER_RESUME_RELEASE);
        assert(worker_resume_action_for(
                   "unbacked-retired", true, "retired-record") ==
               WORKER_RESUME_CANCEL);
        assert(worker_resume_action_for(
                   "reserve-empty", false, NULL) == WORKER_RESUME_CANCEL);

        drive_worker_resume = true;
        mode = GOOD;
        resume_service_state = "allocating";
        resume_service_backed = false;
        object.saved = rebuilt;
        object.has_resource = true;
        object.match = "export-bound";
        object.action = WORKER_RESUME_PUBLISH;
        strcpy(object.last.phase, "export-unknown");
        publishes = 0;
        assert(!worker_resume_apply(
            42, fileno(resume_journal), &resume_config, &client,
            &object, &restored, &did_restore));
        assert(did_restore && restored.occupied && publishes == 1);

        object.saved = restored;
        object.match = "export-bound";
        object.action = WORKER_RESUME_RELEASE;
        strcpy(object.last.phase, "release-intent");
        resume_service_state = "retiring";
        resume_service_backed = true;
        unexports = retires = reclaims = 0;
        did_restore = true;
        assert(!worker_resume_apply(
            42, fileno(resume_journal), &resume_config, &client,
            &object, NULL, &did_restore));
        assert(!did_restore && unexports == 1 && retires == 1 &&
               reclaims == 1 && !strcmp(resume_service_state, "retired"));

        object.saved.exported = (struct obmm_cmd_export){0};
        object.saved.descriptor = (struct mem_service_provider_descriptor){0};
        object.saved.export_no_backing = false;
        object.saved.segment.flags =
            (object.saved.segment.flags & ~OBMM_GSVA_SEG_F_RETIRED) |
            OBMM_GSVA_SEG_F_ACTIVE;
        kernel_record = (struct obmm_cmd_gsva_enumerate_v1){
            .desc = object.saved.segment,
            .resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED,
        };
        object.match = "reserved-no-export";
        object.action = WORKER_RESUME_EXPORT_AND_PUBLISH;
        strcpy(object.last.phase, "reserved");
        resume_service_state = "allocating";
        resume_service_backed = false;
        exports = publishes = 0;
        did_restore = false;
        assert(!worker_resume_apply(
            42, fileno(resume_journal), &resume_config, &client,
            &object, &restored, &did_restore));
        assert(did_restore && exports == 1 && publishes == 1);

        object.saved = restored;
        object.saved.exported = (struct obmm_cmd_export){0};
        object.saved.descriptor = (struct mem_service_provider_descriptor){0};
        object.saved.export_no_backing = true;
        object.saved.segment.flags =
            (object.saved.segment.flags & ~OBMM_GSVA_SEG_F_RETIRED) |
            OBMM_GSVA_SEG_F_ACTIVE;
        kernel_record = (struct obmm_cmd_gsva_enumerate_v1){
            .desc = object.saved.segment,
            .resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED,
        };
        object.match = "reserved-no-export";
        object.action = WORKER_RESUME_CANCEL;
        strcpy(object.last.phase, "export-no-backing");
        resume_service_state = "allocating";
        resume_service_backed = false;
        empty_cancel = true;
        retires = reclaims = cancellations = 0;
        did_restore = true;
        assert(!worker_resume_apply(
            42, fileno(resume_journal), &resume_config, &client,
            &object, NULL, &did_restore));
        assert(!did_restore && retires == 1 && cancellations == 1 &&
               reclaims == 1 && !strcmp(resume_service_state, "retired"));
        empty_cancel = false;
        drive_worker_resume = false;
        resume_service_state = "active";
        resume_service_backed = true;
        memset(&resume_identity, 0, sizeof(resume_identity));
        memset(&resume_service_segment, 0, sizeof(resume_service_segment));
        resume_service_mem_id = resume_service_token_id = 0;
        assert(!fclose(resume_journal));
        puts("gva_worker_partial_resume=pass stages=9 "
             "reserve_intent=isolated torn_frame=isolated");
    }
    {
        char directory[] = "/tmp/gva-worker-loop-XXXXXX";
        char config_path[256], replacement_path[256], state_path[256];
        char archive_path[320];
        char rejoin_config_path[256], rejoin_replacement_path[256];
        char rejoin_state_path[256], rejoin_archive_path[320];
        unsigned before = allocated;
        assert(mkdtemp(directory));
        snprintf(config_path, sizeof(config_path), "%s/worker.conf", directory);
        snprintf(state_path, sizeof(state_path), "%s/worker.state", directory);
        FILE *file = fopen(config_path, "w");
        assert(file);
        fprintf(file, "connect=unix:/unused\nnode_id=nodeA\nincarnation=7\n"
                "readiness_generation=1\nallocation_granularity_bytes=2097152\nstate_file=%s\n",
                state_path);
        assert(!fclose(file));
        drive_worker_loop = true;
        empty_cancel = true;
        mode = GOOD;
        inventory_mode = 8;
        assert(mem_service_provider_obmm_serve_allocations(config_path) == 1);
        assert(access(state_path, F_OK) < 0 && errno == ENOENT);
        assert(allocated == before && !loop_polls);
        inventory_mode = 0;
        assert(!mem_service_provider_obmm_serve_allocations(config_path));
        assert(allocated == before + 1);
        drive_worker_loop = false;
        assert(!mem_service_provider_obmm_inspect_allocation_state(config_path));
        {
            struct worker_config saved;
            struct worker_ledger_entry entry;
            struct stat st;
            unsigned char original_start[WORKER_LEDGER_FRAME_BYTES];
            unsigned char legacy_start[WORKER_LEDGER_FRAME_BYTES];
            unsigned char byte, last;
            struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
            int fd = open(state_path, O_RDWR), status;
            pid_t child;
            bool found_published = false;
            assert(fd >= 0 && !fstat(fd, &st) && !read_config(config_path, &saved));
            for (uint64_t seq = 1; seq <= (uint64_t)st.st_size / WORKER_LEDGER_FRAME_BYTES; seq++) {
                assert(!ledger_read_entry(fd, seq, &saved, &entry));
                if (!strcmp(entry.phase, "published")) {
                    assert(entry.work.key[0] && entry.work.generation);
                    assert(entry.reservation.segment.segment_id && entry.reservation.segment.epoch);
                    assert(entry.reservation.exported.mem_id && entry.reservation.exported.tokenid);
                    assert(entry.reservation.descriptor.len == 96);
                    kernel_record = (struct obmm_cmd_gsva_enumerate_v1){
                        .desc = entry.reservation.segment,
                        .resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED,
                        .export_mem_id = entry.reservation.exported.mem_id,
                        .export_token_id = entry.reservation.exported.tokenid};
                    assert(reconcile_resource(&entry, &entry.reservation, &kernel_record));
                    for (size_t n = 0; n < sizeof(kernel_record.desc); n++) {
                        struct obmm_cmd_gsva_enumerate_v1 bad = kernel_record;
                        ((unsigned char *)&bad.desc)[n] ^= 1;
                        assert(!reconcile_resource(&entry, &entry.reservation, &bad));
                    }
                    {
                        struct worker_ledger_entry retired = entry;
                        struct obmm_cmd_gsva_enumerate_v1 actual = kernel_record;
                        strcpy(retired.phase, "reclaimed");
                        actual.desc.flags = (actual.desc.flags & ~OBMM_GSVA_SEG_F_ACTIVE) |
                                             OBMM_GSVA_SEG_F_RETIRED;
                        actual.resource_flags = actual.export_mem_id = actual.export_token_id = 0;
                        assert(reconcile_resource(&retired, &entry.reservation, &actual));
                        actual.resource_flags = OBMM_GSVA_RESOURCE_ADDRESS_RESERVED;
                        assert(!reconcile_resource(&retired, &entry.reservation, &actual));
                        strcpy(retired.phase, "release-intent");
                        assert(!reconcile_resource(&retired, &entry.reservation, &kernel_record));
                    }
                    found_published = true;
                }
            }
            assert(found_published);
            assert(lseek(fd, 0, SEEK_SET) == 0 && !ledger_read_entry(fd, 1, &saved, &entry));
            saved.kernel_instance[15] ^= 1;
            assert(ledger_read_entry(fd, 2, &saved, &entry));
            saved.kernel_instance[15] ^= 1;
            drive_worker_loop = true;
            for (inventory_mode = 1; inventory_mode <= 8; inventory_mode++) {
                inventory_calls = 0;
                int reconciled = mem_service_provider_obmm_reconcile_allocation_state(config_path);
                assert(reconciled == (inventory_mode == 1 ? 0 : 1));
                assert(inventory_calls);
            }
            inventory_mode = 0;
            drive_worker_loop = false;
            puts("gva_worker_kernel_reconcile=pass cases=8 descriptor_bytes=72 startup_guard=1");
            drive_worker_resume = true;
            inventory_mode = 2;
            assert(mem_service_provider_obmm_resume_allocations(config_path) == 1);
            inventory_mode = 1;
            resume_service_mismatch = true;
            assert(mem_service_provider_obmm_resume_allocations(config_path) == 1);
            resume_service_mismatch = false;
            assert(!mem_service_provider_obmm_resume_allocations(config_path));
            assert(!access(state_path, F_OK));
            drive_worker_resume = false;
            inventory_mode = 0;
            puts("gva_worker_same_kernel_resume=pass reservations=1 "
                 "kernel_mismatch=reject service_mismatch=reject");
            drive_worker_quarantine = true;
            quarantine_status = MEM_SERVICE_WIRE_STATUS_OK;
            unsigned quarantine_before = provider_deregistrations;
            assert(pread(fd, original_start, sizeof(original_start), 0) ==
                   (ssize_t)sizeof(original_start));
            assert(lseek(fd, 0, SEEK_SET) == 0 &&
                   !ledger_read_entry(fd, 1, &saved, &entry));
            entry.config.binding_version = 0;
            assert(!ledger_encode(legacy_start, &entry));
            assert(pwrite(fd, legacy_start, sizeof(legacy_start), 0) ==
                   (ssize_t)sizeof(legacy_start) && !fsync(fd));
            assert(mem_service_provider_obmm_quarantine_allocation_state(
                       config_path) == 1);
            assert(provider_deregistrations == quarantine_before);
            assert(pwrite(fd, original_start, sizeof(original_start), 0) ==
                   (ssize_t)sizeof(original_start) && !fsync(fd));
            assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
            quarantine_status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
            assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
            quarantine_status = MEM_SERVICE_WIRE_STATUS_STALE_REF;
            assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
            assert(provider_deregistrations == quarantine_before + 3);
            saved.incarnation++;
            assert(lseek(fd, 0, SEEK_SET) == 0 && ledger_read_entry(fd, 1, &saved, &entry));
            assert(!fcntl(fd, F_SETLK, &lock));
            child = fork();
            assert(child >= 0);
            if (!child) {
                int inspect_failed =
                    mem_service_provider_obmm_inspect_allocation_state(config_path) == 1;
                int quarantine_failed =
                    mem_service_provider_obmm_quarantine_allocation_state(config_path) == 1;
                _exit(inspect_failed && quarantine_failed ? 0 : 1);
            }
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
            lock.l_type = F_UNLCK;
            assert(!fcntl(fd, F_SETLK, &lock));
            assert(pread(fd, &byte, 1, 48) == 1);
            unsigned char changed = byte ^ 1;
            assert(pwrite(fd, &changed, 1, 48) == 1);
            assert(mem_service_provider_obmm_inspect_allocation_state(config_path) == 1);
            quarantine_before = provider_deregistrations;
            assert(mem_service_provider_obmm_quarantine_allocation_state(config_path) == 1);
            assert(provider_deregistrations == quarantine_before);
            assert(pwrite(fd, &byte, 1, 48) == 1);
            assert(pread(fd, &byte, 1, WORKER_LEDGER_FRAME_BYTES + 48) == 1);
            changed = byte ^ 1;
            assert(pwrite(fd, &changed, 1, WORKER_LEDGER_FRAME_BYTES + 48) == 1);
            quarantine_status = MEM_SERVICE_WIRE_STATUS_OK;
            assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
            assert(pwrite(fd, &byte, 1, WORKER_LEDGER_FRAME_BYTES + 48) == 1);
            assert(pread(fd, &last, 1, st.st_size - 1) == 1);
            assert(!ftruncate(fd, st.st_size - 1));
            assert(mem_service_provider_obmm_inspect_allocation_state(config_path) == 1);
            assert(!mem_service_provider_obmm_quarantine_allocation_state(config_path));
            assert(pwrite(fd, &last, 1, st.st_size - 1) == 1 && !close(fd));
            assert(!mem_service_provider_obmm_inspect_allocation_state(config_path));
            drive_worker_quarantine = false;
            puts("gva_worker_durable_ledger=pass identity=1 corruption=1 torn=1 writer_lock=1");
            puts("gva_worker_quarantine=pass complete=1 invalid_tail=1 torn_tail=1 "
                 "invalid_start=reject legacy_start=reject writer_lock=reject "
                 "idempotent_states=3");
        }
        snprintf(replacement_path, sizeof(replacement_path),
                 "%s/replacement.conf", directory);
        snprintf(archive_path, sizeof(archive_path),
                 "%s.recovered-7-by-8", state_path);
        file = fopen(replacement_path, "w");
        assert(file);
        fprintf(file, "connect=unix:/unused\nnode_id=nodeA\nincarnation=8\n"
                "readiness_generation=2\nallocation_granularity_bytes=2097152\n"
                "state_file=%s\n", state_path);
        assert(!fclose(file));
        drive_worker_holder_fence = true;
        holder_recovery_polls = holder_physical_revokes = holder_receipts = 0;
        holder_physical_fail = true;
        assert(mem_service_provider_obmm_fence_holder_state(
                   config_path, replacement_path) == 1);
        assert(holder_physical_revokes == 1 && !holder_receipts);
        holder_recovery_polls = 0;
        holder_physical_fail = false;
        assert(!mem_service_provider_obmm_fence_holder_state(
            config_path, replacement_path));
        assert(holder_physical_revokes == 2 && holder_receipts == 1);
        drive_worker_holder_fence = false;
        puts("gva_worker_holder_fence=pass physical_first=1 "
             "failed_physical_receipt=0 exact_identity=1");
        drive_worker_terminated_holder_fence = true;
        inventory_mode = 9;
        holder_recovery_polls = holder_physical_revokes = holder_receipts = 0;
        assert(mem_service_provider_obmm_fence_terminated_holder_state(
                   config_path, replacement_path,
                   "00000000000000000000000000000000") == 2);
        assert(mem_service_provider_obmm_fence_terminated_holder_state(
                   config_path, replacement_path,
                   "02000000000000000000000000000000") == 1);
        assert(!holder_physical_revokes && !holder_receipts);
        inventory_mode = 2;
        assert(mem_service_provider_obmm_fence_terminated_holder_state(
                   config_path, replacement_path,
                   "01000000000000000000000000000000") == 1);
        assert(!holder_physical_revokes && !holder_receipts);
        inventory_mode = 9;
        holder_recovery_polls = 0;
        assert(!mem_service_provider_obmm_fence_terminated_holder_state(
            config_path, replacement_path,
            "01000000000000000000000000000000"));
        assert(holder_physical_revokes == 1 && holder_receipts == 1);
        drive_worker_terminated_holder_fence = false;
        inventory_mode = 0;
        puts("gva_worker_terminated_holder_fence=pass old_kernel=required "
             "same_kernel=reject nonempty_inventory=reject route_missing=proof "
             "receipt=1");
        drive_worker_holder_rejoin = true;
        holder_rejoin_home_pending = holder_rejoin_holder_pending = false;
        assert(mem_service_provider_obmm_prepare_holder_rejoin(
                   config_path, replacement_path) == 1);
        snprintf(rejoin_config_path, sizeof(rejoin_config_path),
                 "%s/rejoin-old.conf", directory);
        snprintf(rejoin_replacement_path, sizeof(rejoin_replacement_path),
                 "%s/rejoin-new.conf", directory);
        snprintf(rejoin_state_path, sizeof(rejoin_state_path),
                 "%s/rejoin.state", directory);
        snprintf(rejoin_archive_path, sizeof(rejoin_archive_path),
                 "%s.holder-fenced-7-by-8", rejoin_state_path);
        file = fopen(rejoin_config_path, "w");
        assert(file);
        fprintf(file, "connect=unix:/unused\nnode_id=nodeA\nincarnation=7\n"
                "readiness_generation=1\nallocation_granularity_bytes=2097152\n"
                "state_file=%s\n", rejoin_state_path);
        assert(!fclose(file));
        file = fopen(rejoin_replacement_path, "w");
        assert(file);
        fprintf(file, "connect=unix:/unused\nnode_id=nodeA\nincarnation=8\n"
                "readiness_generation=2\nallocation_granularity_bytes=2097152\n"
                "state_file=%s\n", rejoin_state_path);
        assert(!fclose(file));
        {
            struct worker_config empty_config;
            int empty_ledger;

            assert(!read_config(rejoin_config_path, &empty_config));
            empty_config.kernel_instance[0] = 1;
            empty_ledger = create_state(&empty_config);
            assert(empty_ledger >= 0 && !close(empty_ledger));
        }
        holder_rejoin_home_pending = true;
        assert(mem_service_provider_obmm_prepare_holder_rejoin(
                   rejoin_config_path, rejoin_replacement_path) == 1);
        holder_rejoin_home_pending = false;
        holder_rejoin_holder_pending = true;
        assert(mem_service_provider_obmm_prepare_holder_rejoin(
                   rejoin_config_path, rejoin_replacement_path) == 1);
        holder_rejoin_holder_pending = false;
        assert(!mem_service_provider_obmm_prepare_holder_rejoin(
            rejoin_config_path, rejoin_replacement_path));
        assert(access(rejoin_state_path, F_OK) < 0 &&
               !access(rejoin_archive_path, F_OK));
        assert(!mem_service_provider_obmm_prepare_holder_rejoin(
            rejoin_config_path, rejoin_replacement_path));
        drive_worker_holder_rejoin = false;
        puts("gva_worker_holder_rejoin=pass nonempty=reject home_pending=reject "
             "holder_pending=reject archive=1 retry=1");
        assert(!unlink(rejoin_archive_path) &&
               !unlink(rejoin_replacement_path) &&
               !unlink(rejoin_config_path));
        drive_worker_recovery = true;
        inventory_mode = 3;
        assert(mem_service_provider_obmm_recover_allocation_state(
                   config_path, replacement_path) == 1);
        assert(!access(state_path, F_OK) && access(archive_path, F_OK) < 0);
        inventory_mode = 9;
        recovery_polls = recovery_reclaims = recovery_inspects = 0;
        recovery_home_receipts = 0;
        assert(!mem_service_provider_obmm_recover_allocation_state(
            config_path, replacement_path));
        assert(access(state_path, F_OK) < 0 && !access(archive_path, F_OK));
        assert(recovery_reclaims == 1 && !recovery_inspects &&
               recovery_home_receipts == 1);
        recovery_polls = 0;
        assert(!mem_service_provider_obmm_recover_allocation_state(
            config_path, replacement_path));
        assert(recovery_reclaims == 1 && recovery_inspects == 1);
        drive_worker_recovery = false;
        inventory_mode = 0;
        puts("gva_worker_recovery=pass kernel_replaced=1 "
             "home_holder_fenced=1 service_reclaim=1 archive_retry=1");
        assert(!unlink(archive_path) && !unlink(replacement_path) &&
               !unlink(config_path) && !rmdir(directory));
    }
    run_worker_fault_injection_case("sync-exit", false);
    run_worker_fault_injection_case("torn-exit", true);
    puts("gva_worker_fault_injection=pass reserve_intent=durable "
         "torn_frame=durable quarantine=2");
    fclose(journal);
    puts("gva_worker_transaction=pass");
    return 0;
}
