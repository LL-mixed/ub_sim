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
    ALLOC_EMPTY_IO_ERROR, ALLOC_OUTSIDE_APERTURE, ADDRESS_FULL_JOURNAL_FAIL
};
static enum failure_mode mode;
static unsigned allocated, exports, publishes;
static unsigned unexports, retires, reclaims;
static bool publish_cancelled, empty_cancel;
static bool drive_worker_loop;
static bool drive_worker_recovery;
static bool drive_worker_resume;
static bool resume_service_mismatch;
static bool rollback_empty;
static unsigned cancellations, loop_polls;
static uint64_t kernel_address = 0x700000000000ULL;
static bool fail_next_sync;
static const struct mem_service_client_allocation *direct_work;
static struct mem_service_client_allocation polled_work;
static struct obmm_cmd_gsva_enumerate_v1 kernel_record;
static unsigned inventory_mode, inventory_calls;
static unsigned recovery_polls, recovery_reclaims, recovery_inspects;

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
    if ((drive_worker_loop || drive_worker_recovery || drive_worker_resume) &&
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
    return (drive_worker_loop || drive_worker_recovery || drive_worker_resume) && fd == 42
               ? 0
               : __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long op, ...)
{
    va_list args;
    void *argument;
    assert((drive_worker_loop || drive_worker_recovery || drive_worker_resume) && fd == 42);
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
    assert((drive_worker_loop || drive_worker_recovery || drive_worker_resume) &&
           !strcmp(node, "nodeA"));
    assert(incarnation == (drive_worker_recovery ? 8 : 7));
    assert(readiness_generation == (drive_worker_recovery ? 2 : 1));
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
    assert(drive_worker_recovery && !strcmp(node, "nodeA"));
    assert(current_incarnation == 8 && fenced_incarnation == 7);
    if (!recovery_reclaims && !recovery_polls++) {
        memset(work, 0, sizeof(*work));
        strcpy(work->key, "object");
        strcpy(work->state, "quarantined");
        strcpy(work->home_node, "nodeA");
        work->provider_incarnation = 7;
        work->generation = 99;
        work->size_bytes = 17;
        work->capabilities = 1;
        assert(after_generation == 0);
        *status = MEM_SERVICE_WIRE_STATUS_OK;
        return 0;
    }
    assert(after_generation == (recovery_polls ? 99 : 0));
    *status = MEM_SERVICE_WIRE_STATUS_NOT_FOUND;
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
           generation == 99 && fenced_incarnation == 7 && backing_gone);
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
        struct obmm_cmd_export exported = {
            .length = 1,
            .flags = OBMM_EXPORT_FLAG_GSVA_FIXED_UBA |
                     OBMM_EXPORT_FLAG_ALLOW_MMAP,
            .uba = kernel_record.desc.home_va,
            .mem_id = kernel_record.export_mem_id,
            .tokenid = kernel_record.export_token_id,
        };
        struct mem_service_provider_descriptor descriptor;

        exported.size[0] = kernel_record.desc.size;
        assert(!mem_service_provider_obmm_encode_gsva(
            &kernel_record.desc, &exported, &descriptor));
        memset(out, 0, sizeof(*out));
        strcpy(out->key, key);
        strcpy(out->state, "active");
        strcpy(out->home_node, "nodeA");
        out->provider_incarnation = 7;
        out->generation = 99;
        out->size_bytes = 17;
        out->capabilities = 1;
        out->provider_backed = true;
        out->address = kernel_record.desc.home_va + resume_service_mismatch;
        out->address_len = kernel_record.desc.size;
        out->descriptor_len = descriptor.len;
        memcpy(out->descriptor, descriptor.bytes, descriptor.len);
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
    (void)directory;
    assert((drive_worker_loop || drive_worker_resume) &&
           !strcmp(node, "nodeA") && incarnation == 7);
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_gva_manager_allocate_segment(int fd, struct obmm_cmd_gsva_alloc_segment_v1 *cmd)
{
    assert(fd == 42 && cmd->requested_home_va == 0);
    allocated++;
    if (mode == ADDRESS_FULL || mode == ADDRESS_FULL_JOURNAL_FAIL ||
        mode == ALLOC_COPY_LOST || mode == ALLOC_EMPTY_IO_ERROR) {
        fail_next_sync = mode == ADDRESS_FULL_JOURNAL_FAIL;
        errno = (mode == ADDRESS_FULL || fail_next_sync) ? ENOSPC :
                mode == ALLOC_COPY_LOST ? EFAULT : EIO;
        return -1;
    }
    cmd->desc = (struct obmm_gsva_segment_desc_v1){
        .version = 1, .flags = 7, .segment_id = 9 + allocated,
        .home_va = kernel_address,
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
    if (drive_worker_loop) worker_stop = 1;
    return 0;
}

int __wrap_gva_manager_unexport_segment(int fd,
    const struct obmm_gsva_segment_desc_v1 *segment, const struct obmm_cmd_export *exported)
{
    assert(fd == 42 && segment->segment_id == 9 + allocated && exported->mem_id == 17);
    unexports++;
    return mode == UNEXPORT_FAIL ? -1 : 0;
}

int __wrap_gva_manager_retire_segment(int fd, struct obmm_cmd_gsva_retire_segment_v1 *retire)
{
    assert(fd == 42 && retire->segment_id == 9 + allocated && retire->epoch == 1);
    assert(rollback_empty ? unexports == 0 : unexports == retires + 1);
    retires++;
    return mode == RETIRE_FAIL ? -1 : 0;
}

int __wrap_mem_service_client_reclaim_allocation(
    const struct mem_service_client *client, const char *key, const char *node,
    uint64_t incarnation, uint64_t generation, bool confirmed,
    struct mem_service_client_allocation *out, enum mem_service_wire_status *status)
{
    (void)client;
    assert(!strcmp(key, "object") && !strcmp(node, "nodeA") && incarnation == 7);
    assert(confirmed && (empty_cancel || retires == reclaims + 1));
    reclaims++;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    memset(out, 0, sizeof(*out));
    strcpy(out->state, "retired");
    out->generation = generation + (mode == BAD_RECLAIM_REPLY);
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
    *out = drive_worker_loop ? polled_work : *direct_work;
    strcpy(out->state, "retiring");
    out->generation = generation + (mode == BAD_CANCEL_REPLY);
    if (mode == BAD_CANCEL_BINDING) out->provider_incarnation++;
    if (mode == BACKED_CANCEL_REPLY) out->provider_backed = true;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return mode == CANCEL_FAIL ? -1 : 0;
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
        /* Kernel selection may return a released address. The worker must
         * keep the new identity and reject old cleanup, without a high-water
         * cursor. This boundary fixture does not simulate actual OBMM I/O. */
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
            assert(reservation.segment.home_va == kernel_address);
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
        char directory[] = "/tmp/gva-worker-loop-XXXXXX";
        char config_path[256], replacement_path[256], state_path[256];
        char archive_path[320];
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
            saved.incarnation++;
            assert(lseek(fd, 0, SEEK_SET) == 0 && ledger_read_entry(fd, 1, &saved, &entry));
            assert(!fcntl(fd, F_SETLK, &lock));
            child = fork();
            assert(child >= 0);
            if (!child) _exit(mem_service_provider_obmm_inspect_allocation_state(config_path) == 1 ? 0 : 1);
            assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
            lock.l_type = F_UNLCK;
            assert(!fcntl(fd, F_SETLK, &lock));
            assert(pread(fd, &byte, 1, 48) == 1);
            unsigned char changed = byte ^ 1;
            assert(pwrite(fd, &changed, 1, 48) == 1);
            assert(mem_service_provider_obmm_inspect_allocation_state(config_path) == 1);
            assert(pwrite(fd, &byte, 1, 48) == 1);
            assert(pread(fd, &last, 1, st.st_size - 1) == 1);
            assert(!ftruncate(fd, st.st_size - 1));
            assert(mem_service_provider_obmm_inspect_allocation_state(config_path) == 1);
            assert(pwrite(fd, &last, 1, st.st_size - 1) == 1 && !close(fd));
            assert(!mem_service_provider_obmm_inspect_allocation_state(config_path));
            puts("gva_worker_durable_ledger=pass identity=1 corruption=1 torn=1 writer_lock=1");
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
        drive_worker_recovery = true;
        inventory_mode = 3;
        assert(mem_service_provider_obmm_recover_allocation_state(
                   config_path, replacement_path) == 1);
        assert(!access(state_path, F_OK) && access(archive_path, F_OK) < 0);
        inventory_mode = 9;
        recovery_polls = recovery_reclaims = recovery_inspects = 0;
        assert(!mem_service_provider_obmm_recover_allocation_state(
            config_path, replacement_path));
        assert(access(state_path, F_OK) < 0 && !access(archive_path, F_OK));
        assert(recovery_reclaims == 1 && !recovery_inspects);
        recovery_polls = 0;
        assert(!mem_service_provider_obmm_recover_allocation_state(
            config_path, replacement_path));
        assert(recovery_reclaims == 1 && recovery_inspects == 1);
        drive_worker_recovery = false;
        inventory_mode = 0;
        puts("gva_worker_recovery=pass kernel_replaced=1 service_reclaim=1 archive_retry=1");
        assert(!unlink(archive_path) && !unlink(replacement_path) &&
               !unlink(config_path) && !rmdir(directory));
    }
    fclose(journal);
    puts("gva_worker_transaction=pass");
    return 0;
}
