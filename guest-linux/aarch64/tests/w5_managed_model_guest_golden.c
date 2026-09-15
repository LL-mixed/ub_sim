#include "w5_managed_model_guest.h"
#include "lingqu_shmem_pto.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
struct fake_lease { bool active, begun, complete; } leases[4];
static unsigned acquired, released, polls, enqueues, begins, completes, guards;
static int fail_acquire = -1, fail_begin = -1, fail_complete = -1, fail_release = -1;
static int enqueue_error, poll_error = -EAGAIN;
static uint64_t completion_id = 17;
static uint8_t completion_status = 1;

int lingqu_shmem_mem_service_acquire_owned(struct lingqu_shmem_mem_service_context *c,
    struct mem_service_mapping_owner *o, const struct lingqu_shmem_memref_spec *s,
    struct lingqu_shmem_memref **m, struct lingqu_shmem_mem_service_lease **l)
{
    unsigned i = acquired++;
    CHECK(c && o && i < 4 && s->rank == 1 && s->dtype == 4 && !s->byte_offset);
    CHECK(s->shape[0] == s->byte_length && s->strides[0] == 1);
    leases[i].active = true;
    *l = (void *)&leases[i];
    if ((int)i == fail_acquire) { *m = NULL; return -EIO; }
    *m = (void *)&leases[i];
    return 0;
}
int lingqu_shmem_mem_service_release(struct lingqu_shmem_mem_service_lease *l)
{
    struct fake_lease *f = (void *)l;
    CHECK(f->active && !guards);
    if ((int)(f - leases) == fail_release) { fail_release = -1; return -EIO; }
    CHECK(!f->begun || f->complete);
    f->active = false; ++released; return 0;
}
int lingqu_shmem_mem_service_lease_begin(struct lingqu_shmem_mem_service_lease *l, uint64_t id)
{
    struct fake_lease *f = (void *)l;
    CHECK(id == 17 && f->active && !f->begun);
    if ((int)(f - leases) == fail_begin) { fail_begin = -1; return -EAGAIN; }
    f->begun = true; ++begins; return 0;
}
int lingqu_shmem_mem_service_lease_complete(struct lingqu_shmem_mem_service_lease *l, uint64_t id)
{
    struct fake_lease *f = (void *)l;
    CHECK(id == 17 && f->begun && !f->complete);
    if ((int)(f - leases) == fail_complete) { fail_complete = -1; return -EAGAIN; }
    f->complete = true; ++completes; return 0;
}
int lingqu_shmem_pto_dispatch_prepare(const struct lingqu_shmem_pto_request *r,
    void *metadata, size_t capacity, uint64_t address, LingquPtoDispatchSlotV2 *slot,
    struct lingqu_shmem_pto_wire_result *wire, struct lingqu_shmem_pto_inflight **out)
{
    CHECK(metadata && capacity == 4096 && address == 0x1000);
    CHECK(r->callable_id == 4 && r->op_id == 17 && r->request_id == 19);
    CHECK(r->artifact_fingerprint == 23 && r->memref_count == acquired);
    for (unsigned i = 0; i < acquired; ++i) CHECK(r->memrefs[i].arg_index == i && r->memrefs[i].memref);
    memset(slot, 0, sizeof(*slot)); memset(wire, 0, sizeof(*wire));
    *out = (void *)metadata; ++guards; return 0;
}
void lingqu_shmem_pto_dispatch_finish(struct lingqu_shmem_pto_inflight *p)
{ CHECK(p && guards == 1); --guards; }
int lingqu_shmem_pto_endpoint_enqueue(struct lingqu_shmem_pto_endpoint *e, const LingquPtoDispatchSlotV2 *s)
{ CHECK(e && s && begins == acquired); ++enqueues; return enqueue_error; }
int lingqu_shmem_pto_endpoint_poll(struct lingqu_shmem_pto_endpoint *e, uint64_t id,
    struct lingqu_shmem_pto_completion *c)
{
    CHECK(e && id == 17); ++polls;
    if (poll_error) return poll_error;
    *c = (struct lingqu_shmem_pto_completion){.op_id = completion_id, .status = completion_status};
    return 0;
}

static void reset(void)
{
    memset(leases, 0, sizeof(leases)); acquired = released = polls = enqueues = begins = completes = guards = 0;
    fail_acquire = fail_begin = fail_complete = fail_release = -1;
    enqueue_error = 0; poll_error = -EAGAIN; completion_id = 17; completion_status = 1;
}

int main(int argc, char **argv)
{
    unsigned char control[W5_MANAGED_MODEL_CONTROL_BYTES], metadata[4096];
    uint32_t levels[8] = {0x5133060b, 0, 0, 1, 1, 2, 28, 2048};
    unsigned char prefix[] = {0x51, 0x33, 0x50, 0x52, 0x4f, 0x4d, 0x50, 0x54};
    CHECK(!w5_managed_model_control_encode(control, sizeof(control), 17, 19, levels,
        prefix, sizeof(prefix), NULL, 0));
    if (argc == 2 && !strcmp(argv[1], "--control")) {
        CHECK(fwrite(control, 1, sizeof(control), stdout) == sizeof(control)); return 0;
    }
    unsigned char original[sizeof(control)]; memcpy(original, control, sizeof(control));
    CHECK(w5_managed_model_control_encode(control, sizeof(control), 0, 19, levels, prefix, sizeof(prefix), NULL, 0) == -EINVAL);
    CHECK(!memcmp(original, control, sizeof(control)));
    CHECK(w5_managed_model_control_encode(control, sizeof(control), 17, 19, levels, prefix, sizeof(prefix), NULL, 3) == -EINVAL);
    CHECK(!memcmp(original, control, sizeof(control)));
    for (unsigned inputs = 0; inputs <= 2; ++inputs) for (unsigned fault = 0; fault < 8; ++fault) {
        reset();
        struct mem_service_mapping_owner *owners[4] = {(void *)1, (void *)2, (void *)3, (void *)4};
        uint64_t lengths[4] = {sizeof(control), 64, 128, 4096};
        struct w5_managed_model_operation *op = NULL;
        struct lingqu_shmem_pto_completion completion;
        if (fault == 1) fail_acquire = 1;
        int rc = w5_managed_model_prepare((void *)1, owners, lengths, inputs, 17, 19, 23, 7,
            metadata, sizeof(metadata), 0x1000, &op);
        CHECK(op);
        if (fault == 1) { CHECK(rc == -EIO); CHECK(!w5_managed_model_release(op)); CHECK(released == acquired); continue; }
        CHECK(!rc);
        if (fault == 2) fail_begin = 1;
        if (fault == 3) enqueue_error = -EAGAIN;
        rc = w5_managed_model_submit(op, (void *)1);
        if (fault == 2 || fault == 3) {
            CHECK(rc == -EAGAIN && w5_managed_model_release(op) == -EBUSY);
            enqueue_error = 0;
            CHECK(!w5_managed_model_submit(op, (void *)1));
        } else CHECK(!rc);
        CHECK(w5_managed_model_submit(op, (void *)1) == -EINVAL);
        CHECK(w5_managed_model_poll(op, &completion) == -EAGAIN);
        CHECK(w5_managed_model_release(op) == -EBUSY && !released && guards == 1);
        poll_error = 0;
        if (fault == 4) {
            completion_id = 18;
            CHECK(w5_managed_model_poll(op, &completion) == -EPROTO);
            CHECK(w5_managed_model_release(op) == -EBUSY && !completes);
            completion_id = 17;
        }
        if (fault == 5) fail_complete = 1;
        if (fault == 6) completion_status = 3;
        unsigned before = polls;
        rc = w5_managed_model_poll(op, &completion);
        if (fault == 5) {
            CHECK(rc == -EAGAIN && w5_managed_model_release(op) == -EBUSY);
            CHECK(!w5_managed_model_poll(op, &completion));
        } else CHECK(!rc);
        CHECK(polls == before + 1 && completion.op_id == 17 && completion.status == completion_status);
        CHECK(completes == acquired);
        if (fault == 7) fail_release = 1;
        rc = w5_managed_model_release(op);
        if (fault == 7) { CHECK(rc == -EIO); CHECK(!w5_managed_model_release(op)); }
        else CHECK(!rc);
        CHECK(released == acquired && !guards);
    }
    puts("w5_managed_model_guest=pass cases=24 retained_until_original_completion=1");
    return 0;
}
