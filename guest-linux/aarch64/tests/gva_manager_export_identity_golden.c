/* Exercise the actual CLI diagnostic without opening a device. */
#define main gva_manager_program_main
#include "../apps/gva_manager/gva_manager.c"
#undef main
#include <assert.h>

static unsigned query_calls, export_calls;
static int fault;
static const struct obmm_gsva_segment_desc_v1 reference = {
    .version = OBMM_GSVA_ABI_VERSION, .flags = 7,
    .segment_id = 42, .home_va = 0x700000000000ULL, .size = 2097152,
    .epoch = 17, .home_cna = 7, .owner_node_id = 0, .node_count = 2,
    .cache_policy = GSVA_CACHE_POLICY_WRITE_THROUGH, .p_tag = 2,
    .access_flags = OBMM_GSVA_ACCESS_READ | OBMM_GSVA_ACCESS_WRITE,
    .token_id = 9, .token_value = 19,
};

int __wrap_ioctl(int fd, unsigned long operation, ...)
{
    va_list args;
    void *command;
    assert(fd == 42);
    va_start(args, operation);
    command = va_arg(args, void *);
    va_end(args);
    if (operation == OBMM_CMD_GSVA_QUERY_SEGMENT) {
        struct obmm_cmd_gsva_query_segment_v1 *query = command;
        ++query_calls;
        assert(query->version == OBMM_GSVA_ABI_VERSION && query->segment_id == 42);
        if (fault == 4) { errno = ENOENT; return -1; }
        query->desc = reference;
        if (fault == 3 && query_calls > 1) ++query->desc.token_value;
        return 0;
    }
    assert(operation == OBMM_CMD_GSVA_EXPORT_SEGMENT);
    struct obmm_cmd_gsva_export_segment_v1 *request = command;
    bool bad_header = request->version != OBMM_GSVA_ABI_VERSION ||
                      request->flags || request->reserved;
    ++export_calls;
    /* A valid export could create backing: the diagnostic must never issue it. */
    assert(bad_header || memcmp(&request->segment, &reference, sizeof(reference)));
    if (fault == 2) {
        request->outcome = OBMM_GSVA_EXPORT_CREATED;
        request->export_mem_id = 91;
        return 0;
    }
    errno = fault == 1 ? EIO : bad_header ? EINVAL : ESTALE;
    return -1;
}

int main(void)
{
    struct gva_mgr_config config;
    char *good[] = {"gva_manager", "--check-export-identity", "--segment-id", "42"};
    assert(!parse_args(4, good, &config) && config.check_export_identity && config.segment_id == 42);
    assert(parse_args(2, good, &config) == -EINVAL);
    good[3] = "0";
    assert(parse_args(4, good, &config) == -EINVAL);
    char *mixed[] = {"gva_manager", "--check-export-identity", "--segment-id", "42", "--query"};
    assert(parse_args(5, mixed, &config) == -EINVAL);
    mixed[4] = "--dump-routes";
    assert(parse_args(5, mixed, &config) == -EINVAL);
    for (fault = 0; fault <= 4; ++fault) {
        query_calls = export_calls = 0;
        int result = check_kernel_export_identity(42, reference.segment_id);
        if (!fault) {
            assert(!result && export_calls == 17 && query_calls == 18);
        } else {
            assert(result == -1 && export_calls == (fault == 4 ? 0U : 1U));
        }
    }
    puts("gva_manager_export_identity=pass checks=17 no_valid_export=1");
    return 0;
}
