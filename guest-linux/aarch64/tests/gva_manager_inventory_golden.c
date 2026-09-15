/* Actual library and CLI inventory, with ioctl responses at the kernel seam. */
#define main gva_manager_program_main
#include "../apps/gva_manager/gva_manager.c"
#undef main
#include <assert.h>

static unsigned calls;
static int fault;

int __wrap_ioctl(int fd, unsigned long operation, ...)
{
    va_list args;
    struct obmm_cmd_gsva_enumerate_v1 *q;
    assert(fd == 42 && operation == OBMM_CMD_GSVA_ENUMERATE);
    va_start(args, operation);
    q = va_arg(args, void *);
    va_end(args);
    calls++;
    assert(q->version == 1 && !q->flags && !q->resource_flags && !q->reserved);
    if (fault == 1) { errno = ENOTTY; return -1; }
    if (fault == 2 && calls == 2) { errno = ESTALE; return -1; }
    if (fault == 3) { q->flags = 99; return 0; }
    q->revision = fault == 4 && calls == 2 ? 8 : 7;
    memset(q->kernel_instance, fault == 5 && calls == 2 ? 2 : 1, 16);
    if (calls == 3) {
        q->flags = OBMM_GSVA_ENUM_END;
        if (fault == 6) q->desc.segment_id = 1;
        if (fault == 7) q->cursor++;
        return 0;
    }
    q->flags = OBMM_GSVA_ENUM_ENTRY;
    q->cursor = calls;
    q->desc = (struct obmm_gsva_segment_desc_v1){
        .version = 1, .flags = calls == 1 ? 7 : 11, .segment_id = calls,
        .home_va = 0x700000000000ULL, .size = 2097152, .epoch = calls,
        .home_cna = 7, .node_count = 2, .token_id = calls, .token_value = 19,
    };
    q->resource_flags = calls == 1 ? OBMM_GSVA_RESOURCE_ADDRESS_RESERVED : 0;
    q->export_mem_id = calls == 1 ? 42 : 0;
    q->export_token_id = calls == 1 ? 97 : 0;
    q->aperture_generation = 100;
    if (fault == 8) q->reserved = 1;
    if (fault == 9) q->reserved2 = 1;
    if (fault == 10) q->resource_flags |= 8;
    if (fault == 11) q->desc.flags = 15;
    if (fault == 12) q->cursor = 0;
    if (fault == 13) q->desc.token_value = 0;
    if (fault == 14) q->export_token_id = 0;
    if (fault == 15) q->resource_flags = 0;
    if (fault == 16 && calls == 2) q->resource_flags = 1;
    if (fault == 17) memset(q->kernel_instance, 0, 16);
    if (fault == 18) q->revision = 0;
    if (fault == 19) q->version = 2;
    return 0;
}

int main(void)
{
    struct gva_mgr_config config;
    char *good[] = {"gva_manager", "--dump-segments", "--alloc"};
    assert(!parse_args(2, good, &config) && config.dump_segments);
    assert(parse_args(3, good, &config) == -EINVAL);
    _Static_assert(sizeof(struct obmm_cmd_gsva_enumerate_v1) == 144, "enumeration ABI");
    for (fault = 0; fault <= 19; fault++) {
        FILE *capture = tmpfile();
        int saved = dup(STDOUT_FILENO);
        char output[4096] = {0};
        assert(capture && saved >= 0 && !fflush(stdout));
        assert(dup2(fileno(capture), STDOUT_FILENO) >= 0);
        calls = 0;
        int result = dump_segments(42);
        assert(!fflush(stdout) && !fseek(capture, 0, SEEK_SET));
        size_t bytes = fread(output, 1, sizeof(output) - 1, capture);
        assert(!ferror(capture) && dup2(saved, STDOUT_FILENO) >= 0);
        close(saved);
        fclose(capture);
        if (!fault) {
            assert(!result && calls == 3);
            assert(strstr(output, "cursor=1 segment=1 flags=7"));
            assert(strstr(output, "cursor=2 segment=2 flags=11"));
            assert(strstr(output, "entries=2 revision=7 kernel_instance=01010101010101010101010101010101"));
            assert(strstr(output, "scope=segment-records fence_verified=0"));
        } else {
            assert(result == -1 && !bytes); /* No partial inventory on failure. */
        }
    }
    fault = 1; calls = 0;
    struct obmm_cmd_gsva_enumerate_v1 q = {.version = 1}, saved = q;
    assert(gva_manager_enumerate_segments(42, &q) == -1 && errno == ENOTTY);
    assert(!memcmp(&q, &saved, sizeof(q)));
    q.cursor = 1;
    assert(gva_manager_enumerate_segments(42, &q) == -1 && errno == EINVAL && calls == 1);
    puts("gva_manager_inventory=pass cases=20 no_partial_output=1");
    return 0;
}
