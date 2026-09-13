/* SPDX-License-Identifier: MIT */
/* Exercise the production backend release callback without a device. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "../libs/lingqu_shmem_pto/lingqu_shmem_mem_service_obmm.c"

static int unregister_error;
static unsigned unregister_calls;

int obmm_async_map_unregister(struct obmm_async *runtime,
                              struct obmm_async_map *map)
{
    if (!runtime || !map || map->id != 17 || map->generation != 23) abort();
    ++unregister_calls;
    if (unregister_error) return unregister_error;
    memset(map, 0, sizeof(*map));
    return 0;
}

int main(void)
{
    struct lingqu_shmem_mem_service_obmm_context context = {0};
    struct lingqu_shmem_mem_service_obmm_lease *lease = calloc(1, sizeof(*lease));
    const struct obmm_async_map original = {
        .id = 17, .generation = 23, .local_pa = 0x8000, .length = 4096,
    };
    if (!lease) return 1;
    context.async_runtime = (struct obmm_async *)&context;
    lease->map = original;
    for (unsigned i = 0; i < 2; ++i) {
        unregister_error = i ? -EBUSY : -EIO;
        if (obmm_compute_release(&context, lease) != unregister_error ||
            memcmp(&lease->map, &original, sizeof(original)) != 0 ||
            unregister_calls != i + 1) return 1;
    }
    unregister_error = 0;
    if (obmm_compute_release(&context, lease) != 0 || unregister_calls != 3)
        return 1;
    puts("lingqu_shmem_mem_service_obmm_cleanup=pass attempts=3");
    return 0;
}
