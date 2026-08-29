/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "obmm_common.h"
#include "obmm_async.h"
#include "lingqu_shmem.h"
#include "lingqu_shmem_pto.h"
#include "lingqu_shmem_pto_endpoint.h"
#include "lingqu_shmem_sim.h"

#define PTO_DIRECT_EXPORT_BYTES (2u * 1024u * 1024u)
#define PTO_DIRECT_HOST_VECTOR_ROWS 128u
#define PTO_DIRECT_HOST_VECTOR_COLUMNS 128u
#define PTO_DIRECT_HOST_VECTOR_ELEMENTS \
    (PTO_DIRECT_HOST_VECTOR_ROWS * PTO_DIRECT_HOST_VECTOR_COLUMNS)
#define PTO_DIRECT_DEFAULT_TIMEOUT_MS 120000u
#define PTO_DIRECT_ALIGNMENT 64u
#define PTO_DIRECT_OUTPUT_SENTINEL UINT32_C(0x7fc00001)
#define PTO_DIRECT_CALLABLE_ID UINT64_C(1)

enum pto_direct_role {
    PTO_DIRECT_ROLE_UNSET,
    PTO_DIRECT_ROLE_PRODUCER,
    PTO_DIRECT_ROLE_CONSUMER,
};

struct pto_direct_config {
    enum pto_direct_role role;
    uint32_t node_id;
    uint32_t node_count;
    uint32_t elements;
    uint32_t token_value;
    uint32_t requester_cna;
    uint64_t generation;
    uint64_t artifact_fingerprint;
    uint64_t timeout_ms;
};

struct pto_direct_layout {
    uint64_t input_a_offset;
    uint64_t input_b_offset;
    uint64_t output_offset;
    uint64_t tensor_bytes;
    uint64_t used_bytes;
};

static uint64_t align_up(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static int parse_u64(const char *text, uint64_t *value_out)
{
    char *end = NULL;
    unsigned long long value;

    if (!text || !value_out || text[0] == '-') {
        return -EINVAL;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text) {
        return -EINVAL;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return -EINVAL;
    }
    *value_out = (uint64_t)value;
    return 0;
}

static void usage(FILE *stream)
{
    fprintf(stream,
            "usage: lingqu_shmem_pto_direct --role producer|consumer "
            "--node-id N --node-count N [options]\n"
            "\n"
            "options:\n"
            "  --elements N              f32 elements; callable 1 requires "
            "16384\n"
            "  --generation N            OBMM bootstrap generation\n"
            "  --token-value N           OBMM import token value\n"
            "  --timeout-ms N            producer/dispatch deadline\n"
            "  --requester-cna N         required for consumer\n"
            "  --artifact-fingerprint N  required for consumer\n");
}

static int parse_args(int argc, char **argv, struct pto_direct_config *config)
{
    int index;

    if (!config) {
        return -EINVAL;
    }
    *config = (struct pto_direct_config) {
        .node_count = 2,
        .elements = PTO_DIRECT_HOST_VECTOR_ELEMENTS,
        .generation = 1,
        .timeout_ms = PTO_DIRECT_DEFAULT_TIMEOUT_MS,
    };
    for (index = 1; index < argc; index++) {
        const char *option = argv[index];
        uint64_t value;

        if (strcmp(option, "--help") == 0 || strcmp(option, "-h") == 0) {
            usage(stdout);
            exit(0);
        }
        if (index + 1 >= argc) {
            fprintf(stderr, "missing value for %s\n", option);
            return -EINVAL;
        }
        if (strcmp(option, "--role") == 0) {
            const char *role = argv[++index];

            if (strcmp(role, "producer") == 0) {
                config->role = PTO_DIRECT_ROLE_PRODUCER;
            } else if (strcmp(role, "consumer") == 0) {
                config->role = PTO_DIRECT_ROLE_CONSUMER;
            } else {
                fprintf(stderr, "invalid role: %s\n", role);
                return -EINVAL;
            }
            continue;
        }
        if (parse_u64(argv[++index], &value) != 0) {
            fprintf(stderr, "invalid value for %s: %s\n",
                    option, argv[index]);
            return -EINVAL;
        }
        if (strcmp(option, "--node-id") == 0 && value <= UINT32_MAX) {
            config->node_id = (uint32_t)value;
        } else if (strcmp(option, "--node-count") == 0 &&
                   value <= UINT32_MAX) {
            config->node_count = (uint32_t)value;
        } else if (strcmp(option, "--elements") == 0 &&
                   value <= UINT32_MAX) {
            config->elements = (uint32_t)value;
        } else if (strcmp(option, "--generation") == 0) {
            config->generation = value;
        } else if (strcmp(option, "--token-value") == 0 &&
                   value <= UINT32_MAX) {
            config->token_value = (uint32_t)value;
        } else if (strcmp(option, "--timeout-ms") == 0) {
            config->timeout_ms = value;
        } else if (strcmp(option, "--requester-cna") == 0 &&
                   value <= UINT32_MAX) {
            config->requester_cna = (uint32_t)value;
        } else if (strcmp(option, "--artifact-fingerprint") == 0) {
            config->artifact_fingerprint = value;
        } else {
            fprintf(stderr, "unknown or out-of-range option: %s\n", option);
            return -EINVAL;
        }
    }
    if (config->role == PTO_DIRECT_ROLE_UNSET || config->node_count != 2 ||
        config->node_id >= config->node_count || config->elements == 0 ||
        config->elements != PTO_DIRECT_HOST_VECTOR_ELEMENTS ||
        config->generation == 0 ||
        config->generation > (UINT64_MAX >> 16) ||
        config->timeout_ms == 0 ||
        (config->role == PTO_DIRECT_ROLE_PRODUCER && config->node_id != 0) ||
        (config->role == PTO_DIRECT_ROLE_CONSUMER &&
         (config->node_id != 1 || config->requester_cna == 0 ||
          config->requester_cna > LINGQU_PTO_CNA_MAX ||
          config->artifact_fingerprint == 0))) {
        return -EINVAL;
    }
    return 0;
}

static int build_layout(uint32_t elements, struct pto_direct_layout *layout)
{
    uint64_t tensor_bytes;

    if (!layout || elements == 0 ||
        elements != PTO_DIRECT_HOST_VECTOR_ELEMENTS) {
        return -EINVAL;
    }
    tensor_bytes = (uint64_t)elements * sizeof(float);
    layout->input_a_offset = 0;
    layout->input_b_offset = align_up(tensor_bytes,
                                      PTO_DIRECT_ALIGNMENT);
    layout->output_offset = align_up(layout->input_b_offset + tensor_bytes,
                                     PTO_DIRECT_ALIGNMENT);
    layout->tensor_bytes = tensor_bytes;
    layout->used_bytes = layout->output_offset + tensor_bytes;
    return layout->used_bytes <= PTO_DIRECT_EXPORT_BYTES ? 0 : -E2BIG;
}

static int get_local_cna(uint32_t *cna_out)
{
    static const char *const paths[] = {
        "/sys/bus/ub/devices/00001/primary_cna",
        "/sys/bus/ub/devices/00001/port1/cna",
        "/sys/bus/ub/devices/00001/cna",
    };
    char buffer[64];
    size_t index;

    if (!cna_out) {
        return -EINVAL;
    }
    for (index = 0; index < sizeof(paths) / sizeof(paths[0]); index++) {
        if (obmm_read_file(paths[index], buffer, sizeof(buffer))) {
            uint64_t value;

            if (parse_u64(buffer, &value) == 0 && value != 0 &&
                value <= UINT32_MAX) {
                *cna_out = (uint32_t)value;
                return 0;
            }
        }
    }
    return -ENOENT;
}

static int lookup_producer_meta(int obmm_fd,
                                uint32_t local_cna,
                                uint32_t node_count,
                                uint64_t generation,
                                uint64_t timeout_ms,
                                struct obmm_helpers_meta *meta)
{
    uint64_t started_at = monotonic_ms();

    while (monotonic_ms() - started_at < timeout_ms) {
        struct obmm_cmd_bootstrap_lookup command = {
            .generation = generation,
            .node_count = node_count,
            .local_cna = local_cna,
        };
        uint32_t index;

        if (ioctl(obmm_fd, OBMM_CMD_BOOTSTRAP_LOOKUP, &command) != 0) {
            return -errno;
        }
        for (index = 0; index < command.count; index++) {
            const struct obmm_bootstrap_record *record =
                &command.records[index];

            if (record->node_id != 0) {
                continue;
            }
            *meta = (struct obmm_helpers_meta) {
                .export_mem_id = record->export_mem_id,
                .remote_uba = record->remote_uba,
                .size = record->size,
                .token_id = record->token_id,
                .export_cna = record->export_cna,
            };
            return 0;
        }
        usleep(100000);
    }
    return -ETIMEDOUT;
}

static void seed_region(void *address,
                        const struct pto_direct_layout *layout,
                        uint32_t elements)
{
    float *input_a = (float *)((uint8_t *)address +
                               layout->input_a_offset);
    float *input_b = (float *)((uint8_t *)address +
                               layout->input_b_offset);
    uint32_t *output = (uint32_t *)((uint8_t *)address +
                                    layout->output_offset);
    uint32_t index;

    for (index = 0; index < elements; index++) {
        input_a[index] = (float)index;
        input_b[index] = (float)(2u * index + 1u);
        output[index] = PTO_DIRECT_OUTPUT_SENTINEL;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static bool output_matches(const void *address,
                           const struct pto_direct_layout *layout,
                           uint32_t elements,
                           uint32_t *mismatch_index,
                           uint32_t *expected_bits,
                           uint32_t *actual_bits)
{
    const volatile uint32_t *output =
        (const volatile uint32_t *)((const uint8_t *)address +
                                    layout->output_offset);
    uint32_t index;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (index = 0; index < elements; index++) {
        float sum = (float)(3u * index + 1u);
        float expected = (sum + 1.0f) * (sum + 2.0f);
        uint32_t bits;

        memcpy(&bits, &expected, sizeof(bits));
        if (output[index] != bits) {
            if (mismatch_index) {
                *mismatch_index = index;
            }
            if (expected_bits) {
                *expected_bits = bits;
            }
            if (actual_bits) {
                *actual_bits = output[index];
            }
            return false;
        }
    }
    return true;
}

static int run_producer(const struct pto_direct_config *config,
                        const struct pto_direct_layout *layout,
                        uint32_t local_cna)
{
    struct obmm_helpers_meta meta = { 0 };
    struct obmm_helpers_region region = {
        .fd = -1,
    };
    uint64_t started_at;
    int obmm_fd = -1;
    int rc = 1;

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        fprintf(stderr, "[lingqu_shmem_pto] producer open_obmm error=%s\n",
                strerror(errno));
        return 1;
    }
    if (obmm_do_export(obmm_fd, &meta, PTO_DIRECT_EXPORT_BYTES) != 0) {
        fprintf(stderr, "[lingqu_shmem_pto] producer export error=%s\n",
                strerror(errno));
        goto out;
    }
    meta.export_cna = local_cna;
    if (obmm_map_region(meta.export_mem_id, meta.size, true, &region) != 0) {
        goto out;
    }
    seed_region(region.addr, layout, config->elements);
    if (msync(region.addr, layout->used_bytes, MS_SYNC) != 0) {
        if (errno != EINVAL) {
            fprintf(stderr,
                    "[lingqu_shmem_pto] producer msync error=%s\n",
                    strerror(errno));
            goto out;
        }
        printf("LINGQU_SHMEM_PTO role=producer stage=seeded "
               "msync_unsupported=1 bytes=%" PRIu64 "\n",
               layout->used_bytes);
    } else {
        printf("LINGQU_SHMEM_PTO role=producer stage=seeded "
               "msync_unsupported=0 bytes=%" PRIu64 "\n",
               layout->used_bytes);
    }
    if (obmm_bootstrap_publish(obmm_fd, config->node_id,
                               config->node_count, config->generation,
                               &meta) != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] producer bootstrap_publish error=%s\n",
                strerror(errno));
        goto out;
    }
    printf("LINGQU_SHMEM_PTO role=producer stage=published mem_id=%" PRIu64
           " remote_uba=0x%" PRIx64 " export_cna=0x%x bytes=%" PRIu64
           " elements=%u generation=%" PRIu64 "\n",
           meta.export_mem_id, meta.remote_uba, meta.export_cna,
           meta.size, config->elements, config->generation);

    started_at = monotonic_ms();
    while (monotonic_ms() - started_at < config->timeout_ms) {
        uint32_t mismatch_index = 0;
        uint32_t expected_bits = 0;
        uint32_t actual_bits = 0;

        if (output_matches(region.addr, layout, config->elements,
                           &mismatch_index, &expected_bits, &actual_bits)) {
            printf("LINGQU_SHMEM_PTO role=producer producer_verify=pass "
                   "elements=%u output_offset=%" PRIu64
                   " elapsed_ms=%" PRIu64 "\n",
                   config->elements, layout->output_offset,
                   monotonic_ms() - started_at);
            printf("LINGQU_SHMEM_PTO_RESULT role=producer status=pass\n");
            rc = 0;
            goto out;
        }
        usleep(1000);
    }
    {
        uint32_t mismatch_index = 0;
        uint32_t expected_bits = 0;
        uint32_t actual_bits = 0;

        (void)output_matches(region.addr, layout, config->elements,
                             &mismatch_index, &expected_bits, &actual_bits);
        fprintf(stderr,
                "LINGQU_SHMEM_PTO_RESULT role=producer status=fail "
                "reason=verify_timeout index=%u expected=0x%08x "
                "actual=0x%08x\n",
                mismatch_index, expected_bits, actual_bits);
    }

out:
    obmm_unmap_region(&region);
    if (meta.export_mem_id != 0) {
        (void)obmm_do_unexport(obmm_fd, meta.export_mem_id);
    }
    close(obmm_fd);
    return rc;
}

static int create_memrefs(
    struct lingqu_shmem_region *region,
    const struct pto_direct_layout *layout,
    uint32_t elements,
    struct lingqu_shmem_memref *memrefs[3],
    struct lingqu_shmem_pto_memref_arg args[3])
{
    const uint64_t offsets[3] = {
        layout->input_a_offset,
        layout->input_b_offset,
        layout->output_offset,
    };
    uint32_t index;

    for (index = 0; index < 3; index++) {
        struct lingqu_shmem_memref_spec spec = {
            .byte_offset = offsets[index],
            .byte_length = layout->tensor_bytes,
            .rank = 1,
            .dtype = 0,
            .access = index < 2 ? LINGQU_SHMEM_ACCESS_READ :
                                  LINGQU_SHMEM_ACCESS_WRITE,
            .shape = { elements },
            .strides = { 1 },
        };
        int rc = lingqu_shmem_memref_create(region, &spec,
                                             &memrefs[index]);

        if (rc != 0) {
            return rc;
        }
        args[index].memref = memrefs[index];
        args[index].arg_index = index;
    }
    return 0;
}

static int run_consumer(const struct pto_direct_config *config,
                        const struct pto_direct_layout *layout,
                        uint32_t local_cna)
{
    struct obmm_helpers_meta meta = { 0 };
    struct obmm_helpers_region imported = {
        .fd = -1,
    };
    struct obmm_async_options async_options = {
        .device_path = OBMM_ASYNC_DEFAULT_DEVICE,
        .mode = OBMM_ASYNC_MODE_POLL,
    };
    struct obmm_async *async_runtime = NULL;
    struct obmm_async_map async_map = { 0 };
    struct lingqu_shmem_sim_region_desc region_desc = { 0 };
    struct lingqu_shmem_region *region = NULL;
    struct lingqu_shmem_memref *memrefs[3] = { NULL };
    struct lingqu_shmem_pto_memref_arg args[3] = { 0 };
    struct lingqu_shmem_pto_request request = { 0 };
    struct lingqu_shmem_pto_inflight *inflight = NULL;
    struct lingqu_shmem_pto_wire_result wire_result;
    struct lingqu_shmem_pto_endpoint *endpoint = NULL;
    struct lingqu_shmem_pto_endpoint_info endpoint_info;
    struct lingqu_shmem_pto_completion completion;
    LingquPtoDispatchSlotV2 slot;
    uint64_t local_pas[OBMM_POOL_HELPERS_MAX_NODES] = { 0 };
    bool import_osync[OBMM_POOL_HELPERS_MAX_NODES] = { false };
    uint64_t import_mem_id = 0;
    uint64_t mapping_ref = 0;
    uint8_t *metadata = MAP_FAILED;
    uint64_t metadata_iova = 0;
    uint32_t index;
    int obmm_fd = -1;
    int submit_rc;
    int rc = 1;

    obmm_fd = obmm_open_device();
    if (obmm_fd < 0) {
        return 1;
    }
    submit_rc = lookup_producer_meta(
        obmm_fd, local_cna, config->node_count, config->generation,
        config->timeout_ms, &meta);
    if (submit_rc != 0 || meta.size != PTO_DIRECT_EXPORT_BYTES) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer lookup error=%d size=%" PRIu64
                "\n", submit_rc, meta.size);
        goto out;
    }
    if (!obmm_alloc_import_pas(1, meta.size, local_pas, import_osync,
                               OBMM_IMPORT_CACHE_NC)) {
        fprintf(stderr, "[lingqu_shmem_pto] consumer no_import_pa\n");
        goto out;
    }
    submit_rc = obmm_do_import_v2(
        obmm_fd, &meta, local_cna, local_pas[0], config->token_value,
        OBMM_SIM_DEC_MAP_SOURCE_LEGACY_OBMM,
        OBMM_SIM_DEC_ADDRESS_PROFILE_GENERIC_GVA,
        OBMM_SIM_DEC_CACHE_POLICY_NC,
        0, 0, 0, 0, 0, 0, 0, 0, 0, &import_mem_id);
    if (submit_rc != 0) {
        fprintf(stderr, "[lingqu_shmem_pto] consumer import error=%s\n",
                strerror(errno));
        goto out;
    }
    submit_rc = obmm_map_region(import_mem_id, meta.size,
                                import_osync[0], &imported);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer import_map error=%s\n",
                strerror(errno));
        goto out;
    }
    printf("LINGQU_SHMEM_PTO role=consumer stage=import_mapped "
           "import_mem_id=%" PRIu64 " local_pa=0x%" PRIx64
           " bytes=%" PRIu64 "\n",
           import_mem_id, local_pas[0], meta.size);
    submit_rc = obmm_async_open(&async_runtime, &async_options);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer async_open error=%d\n",
                submit_rc);
        goto out;
    }
    submit_rc = obmm_async_map_register(async_runtime, obmm_fd,
                                        import_mem_id, imported.addr,
                                        meta.size, &async_map);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer map_register error=%d\n",
                submit_rc);
        goto out;
    }
    submit_rc = lingqu_shmem_pto_obmm_mapping_ref(
        async_map.id, async_map.generation, &mapping_ref);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer mapping_ref error=%d\n",
                submit_rc);
        goto out;
    }
    region_desc = (struct lingqu_shmem_sim_region_desc) {
        .mapped_addr = imported.addr,
        .mapped_length = meta.size,
        .ub_gm_addr = local_pas[0],
        .opaque_mapping_ref = mapping_ref,
    };
    if (lingqu_shmem_sim_region_create(&region_desc, &region) != 0 ||
        create_memrefs(region, layout, config->elements, memrefs, args) != 0) {
        fprintf(stderr, "[lingqu_shmem_pto] consumer memref_create failed\n");
        goto out;
    }
    metadata = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (metadata == MAP_FAILED) {
        goto out;
    }
    memset(metadata, 0, 4096);
    if (lingqu_shmem_sim_phys_for_virt(metadata, &metadata_iova) != 0) {
        goto out;
    }
    request = (struct lingqu_shmem_pto_request) {
        .op_id = (config->generation << 16) | UINT64_C(1),
        .request_id = (config->generation << 16) | UINT64_C(2),
        .callable_id = PTO_DIRECT_CALLABLE_ID,
        .artifact_fingerprint = config->artifact_fingerprint,
        .requester_cna = config->requester_cna,
        .memrefs = args,
        .memref_count = 3,
    };
    if (lingqu_shmem_pto_dispatch_prepare(
            &request, metadata, 4096, metadata_iova, &slot,
            &wire_result, &inflight) != 0) {
        fprintf(stderr, "[lingqu_shmem_pto] consumer prepare failed\n");
        goto out;
    }
    submit_rc = lingqu_shmem_pto_endpoint_open(&endpoint, &endpoint_info);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer endpoint_open error=%d\n",
                submit_rc);
        goto out;
    }
    printf("LINGQU_SHMEM_PTO role=consumer stage=prepared import_mem_id=%"
           PRIu64 " local_pa=0x%" PRIx64 " map_id=%" PRIu64
           " map_generation=%" PRIu64 " mapping_ref=0x%" PRIx64
           " metadata_iova=0x%" PRIx64 " metadata_bytes=%zu crc=0x%08x"
           " requester_cna=0x%x fingerprint=0x%" PRIx64
           " resource=%s\n",
           import_mem_id, local_pas[0], async_map.id,
           async_map.generation, mapping_ref, metadata_iova,
           wire_result.metadata_bytes, wire_result.metadata_crc32,
           config->requester_cna, config->artifact_fingerprint,
           endpoint_info.resource_path);
    submit_rc = lingqu_shmem_pto_endpoint_submit(
        endpoint, &slot, config->timeout_ms, &completion);
    if (submit_rc != 0) {
        fprintf(stderr,
                "LINGQU_SHMEM_PTO_RESULT role=consumer status=fail "
                "reason=submit rc=%d\n", submit_rc);
        goto out;
    }
    printf("LINGQU_SHMEM_PTO role=consumer stage=completion op_id=%" PRIu64
           " source=%u completion_status=%u error=%s finished_at=%" PRIu64
           "\n", completion.op_id, completion.source, completion.status,
           completion.error_code[0] ? completion.error_code : "none",
           completion.finished_at);
    if (!lingqu_shmem_pto_completion_succeeded(&completion)) {
        fprintf(stderr,
                "LINGQU_SHMEM_PTO_RESULT role=consumer status=fail "
                "reason=completion error=%s\n",
                completion.error_code[0] ? completion.error_code : "unknown");
        goto out;
    }
    printf("LINGQU_SHMEM_PTO_RESULT role=consumer status=pass "
           "op_id=%" PRIu64 " elements=%u\n",
           completion.op_id, config->elements);
    rc = 0;

out:
    lingqu_shmem_pto_endpoint_close(endpoint);
    lingqu_shmem_pto_dispatch_finish(inflight);
    for (index = 0; index < 3; index++) {
        if (memrefs[index]) {
            (void)lingqu_shmem_memref_destroy(memrefs[index]);
        }
    }
    if (region) {
        (void)lingqu_shmem_sim_region_destroy(region);
    }
    if (async_map.id != 0) {
        (void)obmm_async_map_unregister(async_runtime, &async_map);
    }
    obmm_async_close(async_runtime);
    if (metadata != MAP_FAILED) {
        munmap(metadata, 4096);
    }
    obmm_unmap_region(&imported);
    if (import_mem_id != 0) {
        (void)obmm_do_unimport(obmm_fd, import_mem_id);
    }
    if (obmm_fd >= 0) {
        close(obmm_fd);
    }
    return rc;
}

int main(int argc, char **argv)
{
    struct pto_direct_config config;
    struct pto_direct_layout layout;
    uint32_t local_cna = 0;

    if (parse_args(argc, argv, &config) != 0 ||
        build_layout(config.elements, &layout) != 0) {
        usage(stderr);
        return 2;
    }
    if (get_local_cna(&local_cna) != 0) {
        fprintf(stderr, "[lingqu_shmem_pto] cannot read local CNA\n");
        return 1;
    }
    printf("LINGQU_SHMEM_PTO role=%s stage=start node_id=%u node_count=%u "
           "local_cna=0x%x elements=%u generation=%" PRIu64 "\n",
           config.role == PTO_DIRECT_ROLE_PRODUCER ? "producer" :
                                                     "consumer",
           config.node_id, config.node_count, local_cna,
           config.elements, config.generation);
    if (config.role == PTO_DIRECT_ROLE_PRODUCER) {
        return run_producer(&config, &layout, local_cna);
    }
    return run_consumer(&config, &layout, local_cna);
}
