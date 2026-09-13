/* Executable worker transaction test; kernel and publish calls are wrapped. */
#include "components/mem_service/providers/mem_service_provider_obmm_worker.c"
#include <assert.h>
#include <stdarg.h>

enum failure_mode {
    GOOD, ALLOC_FAIL, EXPORT_FAIL, PUBLISH_FAIL, BAD_REPLY,
    UNEXPORT_FAIL, RETIRE_FAIL, RECLAIM_FAIL, BAD_RECLAIM_REPLY,
    CANCEL_FAIL, BAD_CANCEL_REPLY, BAD_CANCEL_BINDING, BACKED_CANCEL_REPLY,
    BACKING_EMPTY
};
static enum failure_mode mode;
static unsigned allocated, exports, publishes;
static unsigned unexports, retires, reclaims;
static bool publish_cancelled, empty_cancel;
static bool drive_worker_loop;
static bool rollback_empty;
static unsigned cancellations, loop_polls;

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);
int __wrap_open(const char *path, int flags, ...)
{
    mode_t create_mode = 0;
    if (drive_worker_loop && !strcmp(path, "/dev/obmm")) return 42;
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
    return drive_worker_loop && fd == 42 ? 0 : __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long op, ...)
{
    va_list args;
    struct obmm_cmd_gsva_aperture *aperture;
    assert(drive_worker_loop && fd == 42 && op == OBMM_CMD_GSVA_APERTURE_QUERY);
    va_start(args, op);
    aperture = va_arg(args, struct obmm_cmd_gsva_aperture *);
    va_end(args);
    *aperture = (struct obmm_cmd_gsva_aperture){.base = 0x700000000000ULL,
        .size = 0x1000000, .node_count = 2, .flags = OBMM_GSVA_APERTURE_F_ACTIVE};
    return 0;
}

int __wrap_mem_service_client_provider_refresh(const struct mem_service_client *client,
    const char *node, uint64_t incarnation, uint64_t readiness_generation,
    struct mem_service_client_provider_directory *directory, enum mem_service_wire_status *status)
{
    (void)client;
    assert(drive_worker_loop && !strcmp(node, "nodeA") && incarnation == 7 && readiness_generation == 1);
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
    assert(drive_worker_loop && !strcmp(node, "nodeA") && incarnation == 7 && !after_generation);
    *work = (struct mem_service_client_allocation){.key = "object", .state = "allocating",
        .home_node = "nodeA", .provider_incarnation = 7, .generation = 99,
        .size_bytes = 17, .capabilities = 1};
    if (loop_polls++ == 0) {
        work->generation = 98;
        work->size_bytes = UINT64_MAX;
    }
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_mem_service_client_provider_deregister(const struct mem_service_client *client,
    const char *node, uint64_t incarnation, struct mem_service_client_provider_directory *directory,
    enum mem_service_wire_status *status)
{
    (void)client;
    (void)directory;
    assert(drive_worker_loop && !strcmp(node, "nodeA") && incarnation == 7);
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return 0;
}

int __wrap_gva_manager_allocate_segment(int fd, struct obmm_cmd_gsva_alloc_segment_v1 *cmd)
{
    assert(fd == 42);
    allocated++;
    cmd->desc = (struct obmm_gsva_segment_desc_v1){
        .version = 1, .flags = 7, .segment_id = 9, .home_va = cmd->requested_home_va,
        .size = cmd->size, .epoch = 1, .home_cna = 7, .owner_node_id = 0,
        .node_count = 2, .cache_policy = cmd->cache_policy, .access_flags = 3,
        .token_id = 2, .token_value = 3,
    };
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
    assert(fd == 42 && segment->segment_id == 9 && exported->mem_id == 17);
    unexports++;
    return mode == UNEXPORT_FAIL ? -1 : 0;
}

int __wrap_gva_manager_retire_segment(int fd, struct obmm_cmd_gsva_retire_segment_v1 *retire)
{
    assert(fd == 42 && retire->segment_id == 9 && retire->epoch == 1);
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
    *out = (struct mem_service_client_allocation){.key = "object", .state = "retiring",
        .home_node = "nodeA", .provider_incarnation = 7,
        .generation = generation + (mode == BAD_CANCEL_REPLY)};
    if (mode == BAD_CANCEL_BINDING) out->provider_incarnation++;
    if (mode == BACKED_CANCEL_REPLY) out->provider_backed = true;
    *status = MEM_SERVICE_WIRE_STATUS_OK;
    return mode == CANCEL_FAIL ? -1 : 0;
}

int main(void)
{
    struct worker_config config = {.node = "nodeA", .incarnation = 7,
                                  .allocation_granularity_bytes = 2097152};
    struct mem_service_client client = {0};
    struct obmm_cmd_gsva_aperture aperture = {.base = 0x700000000000ULL, .size = 0x10000000};
    struct mem_service_client_allocation work = {
        .key = "object", .state = "allocating", .home_node = "nodeA",
        .generation = 1, .size_bytes = 65536, .alignment_bytes = 65536,
        .capabilities = 1, .provider_incarnation = 7,
    };
    FILE *journal = tmpfile();
    struct worker_reservation reservation = {0};
    uint64_t next_address = aperture.base;
    assert(journal);
    for (mode = GOOD; mode <= BAD_REPLY; mode++) {
        unsigned old_alloc = allocated, old_export = exports, old_publish = publishes;
        memset(&reservation, 0, sizeof(reservation));
        int rc = allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                               &reservation, &next_address);
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
                             &reservation, &next_address) != 0);
        strcpy(work.state, "retiring");
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) != 0);
        assert(allocated == before);
    }
    for (int failure = GOOD; failure <= BAD_RECLAIM_REPLY; failure++) {
        struct mem_service_client_allocation release = {0};
        uint64_t previous_address = next_address;
        mode = GOOD;
        memset(&reservation, 0, sizeof(reservation));
        strcpy(work.state, "allocating");
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) == 0);
        assert(reservation.segment.home_va >= previous_address);
        assert(reservation.segment.size == config.allocation_granularity_bytes);
        assert(next_address == reservation.segment.home_va + reservation.segment.size);
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
        uint64_t before_address = next_address;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) == 1);
        assert(!reservation.occupied && next_address == before_address);
        assert(allocated == before);
        work.size_bytes = aperture.size + 1;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) == 1);
        assert(!reservation.occupied && next_address == before_address);
        assert(allocated == before);
        work.size_bytes = 2097153;
        assert(!allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                              &reservation, &next_address));
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
                uint64_t previous = next_address;
                uint64_t alignment = alignments[j] < 2097152 ? 2097152 : alignments[j];
                memset(&reservation, 0, sizeof(reservation));
                work.size_bytes = sizes[i];
                work.alignment_bytes = alignments[j];
                assert(!allocate_work(42, fileno(journal), &config, &client, &aperture,
                                      &work, &reservation, &next_address));
                assert(reservation.segment.home_va >= previous);
                assert(!(reservation.segment.home_va & (alignment - 1)));
                assert(reservation.segment.size == ((sizes[i] + 2097151) & ~2097151ULL));
                assert(next_address == reservation.segment.home_va + reservation.segment.size);
            }
        }
        unsigned before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        work.alignment_bytes = 3;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) == WORKER_ALLOCATE_ERROR);
        work.alignment_bytes = 1ULL << 63;
        assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                             &reservation, &next_address) == WORKER_ALLOCATE_CAPACITY);
        assert(allocated == before && !reservation.occupied);
        work.alignment_bytes = 65536;
    }
    {
        unsigned before = allocated;
        memset(&reservation, 0, sizeof(reservation));
        work.size_bytes = 17;
        publish_cancelled = true;
        assert(!allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                              &reservation, &next_address));
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
            uint64_t previous_address = next_address;
            memset(&reservation, 0, sizeof(reservation));
            strcpy(work.state, "allocating");
            mode = BACKING_EMPTY;
            assert(allocate_work(42, fileno(journal), &config, &client, &aperture, &work,
                                 &reservation, &next_address) == WORKER_ALLOCATE_BACKING_EMPTY);
            assert(reservation.occupied && reservation.export_no_backing);
            assert(next_address > previous_address && publishes == before_publish);
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
        char directory[] = "/tmp/gva-worker-loop-XXXXXX";
        char config_path[256], state_path[256];
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
        mode = GOOD;
        assert(!mem_service_provider_obmm_serve_allocations(config_path));
        assert(allocated == before + 1);
        drive_worker_loop = false;
        assert(!unlink(state_path) && !unlink(config_path) && !rmdir(directory));
    }
    fclose(journal);
    puts("gva_worker_transaction=pass");
    return 0;
}
