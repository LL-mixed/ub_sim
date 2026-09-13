/* SPDX-License-Identifier: MIT */
#define ioctl test_ioctl
#include "libs/obmm_async/obmm_async.c"
#undef ioctl

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

static unsigned calls;
static int device_error;
static bool invalid_response;

int test_ioctl(int fd, unsigned long command, ...)
{
    va_list args;
    void *request;

    assert(fd == 7);
    calls++;
    va_start(args, command);
    request = va_arg(args, void *);
    va_end(args);
    if (command == OBMM_ASYNC_IOCTL_MAP_REGISTER_V2) {
        struct obmm_async_map_register_v2 *map = request;

        assert(map->flags == OBMM_ASYNC_MAP_STRICT_PTO);
        assert(map->reserved == 0 && map->mem_id == 9);
        assert(map->mapped_addr == 0x700000000000 && map->length == 4096);
        if (device_error) {
            errno = device_error;
            return -1;
        }
        map->map_id = 3;
        map->map_generation = 5;
        map->local_pa = invalid_response ? 0 : 0x40000000000;
        return 0;
    }
    assert(command == OBMM_ASYNC_IOCTL_MAP_UNREGISTER);
    assert(((struct obmm_async_map_unregister_v1 *)request)->map_id == 3);
    assert(((struct obmm_async_map_unregister_v1 *)request)->map_generation == 5);
    return 0;
}

int main(void)
{
    struct obmm_async runtime = {
        .fd = 7, .queue_mapping = (void *)1, .buffer_mapping = (void *)1,
    };
    struct obmm_async_map map = { 0 };
    void *base = (void *)(uintptr_t)0x700000000000;
    const int rejected[] = { ENOTTY, EINVAL, EIO, EACCES };
    unsigned i;

    for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        device_error = rejected[i];
        assert(obmm_async_map_register_strict_pto(
            &runtime, 8, 9, base, 4096, &map) == -rejected[i]);
        assert(calls == i + 1 && !map.id && !map.local_pa);
    }
    device_error = 0;
    assert(obmm_async_map_register_strict_pto(
        &runtime, 8, 9, base, 4096, &map) == 0);
    assert(calls == 5 && map.id == 3 && map.generation == 5);
    assert(map.local_pa == 0x40000000000 && map.length == 4096);
    assert(obmm_async_map_register_strict_pto(
        &runtime, 8, 9, base, 4096, &map) == -EINVAL);
    assert(calls == 5);
    assert(obmm_async_map_unregister(&runtime, &map) == 0);
    assert(!map.id && !map.local_pa && !map.length && calls == 6);
    invalid_response = true;
    assert(obmm_async_map_register_strict_pto(
        &runtime, 8, 9, base, 4096, &map) == -EPROTO);
    assert(map.id == 3 && map.generation == 5 && calls == 7);
    assert(obmm_async_map_unregister(&runtime, &map) == 0 && calls == 8);
    assert(obmm_async_map_register_strict_pto(
        &runtime, 8, 9, (void *)(uintptr_t)UINT64_MAX, 2, &map) == -EINVAL);
    assert(obmm_async_map_register_strict_pto(
        &runtime, 8, 0, base, 4096, &map) == -EINVAL);
    assert(calls == 8);
    puts("obmm_async_strict_pto_golden=pass calls=8 legacy_retries=0");
    return 0;
}
