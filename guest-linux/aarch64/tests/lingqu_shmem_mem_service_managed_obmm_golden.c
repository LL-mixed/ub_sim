/* SPDX-License-Identifier: MIT */
/* Production managed adapter with provider/ioctl boundaries replaced. */
#define LINGQU_SHMEM_MANAGED_MAPPING
#include "../libs/lingqu_shmem_pto/lingqu_shmem_mem_service_obmm.c"
#include <assert.h>
#include <stdio.h>

struct mem_service_provider_obmm_mapping_pin { bool live; };
static struct mem_service_provider_obmm_mapping_pin pin;
static unsigned registrations, unregisters, releases;
static int pin_error, register_error, unregister_error, release_error;
static uint8_t payload[256];

int mem_service_provider_obmm_mapping_pin_acquire(
    const struct mem_service_provider_mapping_binding *binding, uint64_t access,
    struct mem_service_provider_obmm_mapping_pin **out,
    struct mem_service_provider_obmm_pinned_mapping *view)
{
    assert(binding && binding->mapping.base == payload && access == 1 && !*out && !pin.live);
    if (pin_error) return pin_error;
    pin.live = true;
    *out = &pin;
    *view = (struct mem_service_provider_obmm_pinned_mapping) {
        .obmm_fd = 7, .mem_id = 31, .base = payload, .len = sizeof(payload), .access_flags = access,
    };
    return 0;
}

int mem_service_provider_obmm_mapping_pin_release(
    struct mem_service_provider_obmm_mapping_pin **inout)
{
    assert(*inout == &pin && pin.live);
    ++releases;
    if (release_error) return release_error;
    pin.live = false;
    *inout = NULL;
    return 0;
}

int obmm_async_map_register_strict_pto(struct obmm_async *runtime, int fd,
    uint64_t mem_id, void *base, uint64_t len, struct obmm_async_map *map)
{
    assert(runtime && fd == 7 && mem_id == 31 && base == payload && len == sizeof(payload));
    assert(pin.live && !map->id);
    ++registrations;
    if (!register_error || register_error == -EPROTO) {
        *map = (struct obmm_async_map) {
            .id = 17, .generation = 23, .length = len,
            .local_pa = register_error ? 0 : 0x8000,
        };
    }
    return register_error;
}

int obmm_async_map_unregister(struct obmm_async *runtime, struct obmm_async_map *map)
{
    assert(runtime && map->id == 17 && map->generation == 23 && pin.live);
    ++unregisters;
    if (unregister_error) return unregister_error;
    memset(map, 0, sizeof(*map));
    return 0;
}

int main(void)
{
    struct lingqu_shmem_mem_service_obmm_context context = {0};
    struct mem_service_client_object_mapping mapping = {0};
    struct lingqu_shmem_mem_service_region_binding binding;
    context.async_runtime = (struct obmm_async *)&context;
    mapping.binding.mapping.base = payload;
    assert(!obmm_compute_acquire_managed(&context, &mapping, 1, &binding));
    assert(binding.backend_lease && binding.region.mapped_addr == payload &&
           binding.region.mapped_length == sizeof(payload) && binding.region.ub_gm_addr == 0x8000 &&
           binding.region.opaque_mapping_ref && !binding.object_byte_offset);
    unregister_error = -EIO;
    assert(obmm_compute_release(&context, binding.backend_lease) == -EIO);
    assert(pin.live && !releases && unregisters == 1);
    unregister_error = 0;
    release_error = -EAGAIN;
    assert(obmm_compute_release(&context, binding.backend_lease) == -EAGAIN);
    assert(pin.live && releases == 1 && unregisters == 2);
    release_error = 0;
    assert(!obmm_compute_release(&context, binding.backend_lease));
    assert(!pin.live && releases == 2 && unregisters == 2);
    for (unsigned fault = 0; fault < 2; ++fault) {
        register_error = fault ? -EPROTO : -EOPNOTSUPP;
        assert(obmm_compute_acquire_managed(&context, &mapping, 1, &binding) == register_error);
        assert(binding.backend_lease && pin.live && !binding.region.mapped_addr);
        assert(!obmm_compute_release(&context, binding.backend_lease));
        assert(!pin.live);
    }
    pin_error = -EACCES;
    assert(obmm_compute_acquire_managed(&context, &mapping, 1, &binding) == -EACCES);
    assert(!binding.backend_lease && !pin.live && registrations == 3 && unregisters == 3);
    puts("managed_obmm_attachment=pass strict_registration=1 retained_pin=1");
    return 0;
}
