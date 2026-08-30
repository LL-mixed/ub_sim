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
#define PTO_DIRECT_COMPLETION_FAILED 3u
#define PTO_DIRECT_METADATA_BYTES 4096u

enum pto_direct_role {
    PTO_DIRECT_ROLE_UNSET,
    PTO_DIRECT_ROLE_PRODUCER,
    PTO_DIRECT_ROLE_CONSUMER,
};

enum pto_direct_expectation {
    PTO_DIRECT_EXPECT_SUCCESS,
    PTO_DIRECT_EXPECT_AUTHORIZATION_TIMEOUT,
    PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED,
    PTO_DIRECT_EXPECT_BAD_MEMREF,
    PTO_DIRECT_EXPECT_ACCESS_DENIED,
};

enum pto_direct_fault_case {
    PTO_DIRECT_FAULT_NONE,
    PTO_DIRECT_FAULT_BAD_MAPPING_REF,
    PTO_DIRECT_FAULT_STALE_MAPPING,
    PTO_DIRECT_FAULT_RELEASED_IMPORT,
    PTO_DIRECT_FAULT_WRONG_REQUESTER,
    PTO_DIRECT_FAULT_OOB,
    PTO_DIRECT_FAULT_ADDRESS_OVERFLOW,
    PTO_DIRECT_FAULT_ROLE_ACCESS_MISMATCH,
    PTO_DIRECT_FAULT_TSTORE_ON_READ,
    PTO_DIRECT_FAULT_TLOAD_ON_WRITE,
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
    uint64_t cancel_after_ms;
    enum pto_direct_expectation expectation;
    enum pto_direct_fault_case fault_case;
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

static const char *expectation_name(enum pto_direct_expectation expectation)
{
    switch (expectation) {
    case PTO_DIRECT_EXPECT_SUCCESS:
        return "success";
    case PTO_DIRECT_EXPECT_AUTHORIZATION_TIMEOUT:
        return "authorization-timeout";
    case PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED:
        return "authorization-cancelled";
    case PTO_DIRECT_EXPECT_BAD_MEMREF:
        return "bad-memref";
    case PTO_DIRECT_EXPECT_ACCESS_DENIED:
        return "access-denied";
    }
    return "unknown";
}

static const char *fault_case_name(enum pto_direct_fault_case fault_case)
{
    switch (fault_case) {
    case PTO_DIRECT_FAULT_NONE:
        return "none";
    case PTO_DIRECT_FAULT_BAD_MAPPING_REF:
        return "bad-mapping-ref";
    case PTO_DIRECT_FAULT_STALE_MAPPING:
        return "stale-mapping";
    case PTO_DIRECT_FAULT_RELEASED_IMPORT:
        return "released-import";
    case PTO_DIRECT_FAULT_WRONG_REQUESTER:
        return "wrong-requester";
    case PTO_DIRECT_FAULT_OOB:
        return "oob";
    case PTO_DIRECT_FAULT_ADDRESS_OVERFLOW:
        return "address-overflow";
    case PTO_DIRECT_FAULT_ROLE_ACCESS_MISMATCH:
        return "role-access-mismatch";
    case PTO_DIRECT_FAULT_TSTORE_ON_READ:
        return "tstore-on-read";
    case PTO_DIRECT_FAULT_TLOAD_ON_WRITE:
        return "tload-on-write";
    }
    return "unknown";
}

static enum pto_direct_expectation fault_case_expectation(
    enum pto_direct_fault_case fault_case)
{
    if (fault_case == PTO_DIRECT_FAULT_WRONG_REQUESTER ||
        fault_case == PTO_DIRECT_FAULT_TSTORE_ON_READ ||
        fault_case == PTO_DIRECT_FAULT_TLOAD_ON_WRITE) {
        return PTO_DIRECT_EXPECT_ACCESS_DENIED;
    }
    if (fault_case != PTO_DIRECT_FAULT_NONE) {
        return PTO_DIRECT_EXPECT_BAD_MEMREF;
    }
    return PTO_DIRECT_EXPECT_SUCCESS;
}

static const char *expectation_error_code(
    enum pto_direct_expectation expectation)
{
    switch (expectation) {
    case PTO_DIRECT_EXPECT_AUTHORIZATION_TIMEOUT:
        return LINGQU_PTO_UB_GM_CODE_AUTHORIZATION_TIMEOUT;
    case PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED:
        return LINGQU_PTO_UB_GM_CODE_AUTHORIZATION_CANCELLED;
    case PTO_DIRECT_EXPECT_BAD_MEMREF:
        return LINGQU_PTO_UB_GM_CODE_BAD_MEMREF;
    case PTO_DIRECT_EXPECT_ACCESS_DENIED:
        return LINGQU_PTO_UB_GM_CODE_ACCESS_DENIED;
    case PTO_DIRECT_EXPECT_SUCCESS:
        break;
    }
    return NULL;
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
            "  --cancel-after-ms N       cancel pending authorization; "
            "zero disables\n"
            "  --requester-cna N         required for consumer\n"
            "  --artifact-fingerprint N  required for consumer\n"
            "  --expect OUTCOME          success, authorization-timeout, "
            "authorization-cancelled, bad-memref, or access-denied\n"
            "  --fault-case CASE         none, bad-mapping-ref, "
            "stale-mapping, released-import, wrong-requester, oob, "
            "address-overflow, "
            "role-access-mismatch, tstore-on-read, or tload-on-write\n");
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
        if (strcmp(option, "--expect") == 0) {
            const char *expectation = argv[++index];

            if (strcmp(expectation, "success") == 0) {
                config->expectation = PTO_DIRECT_EXPECT_SUCCESS;
            } else if (strcmp(expectation,
                              "authorization-timeout") == 0) {
                config->expectation =
                    PTO_DIRECT_EXPECT_AUTHORIZATION_TIMEOUT;
            } else if (strcmp(expectation,
                              "authorization-cancelled") == 0) {
                config->expectation =
                    PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED;
            } else if (strcmp(expectation, "bad-memref") == 0) {
                config->expectation = PTO_DIRECT_EXPECT_BAD_MEMREF;
            } else if (strcmp(expectation, "access-denied") == 0) {
                config->expectation = PTO_DIRECT_EXPECT_ACCESS_DENIED;
            } else {
                fprintf(stderr, "invalid expected outcome: %s\n",
                        expectation);
                return -EINVAL;
            }
            continue;
        }
        if (strcmp(option, "--fault-case") == 0) {
            const char *fault_case = argv[++index];

            if (strcmp(fault_case, "none") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_NONE;
            } else if (strcmp(fault_case, "bad-mapping-ref") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_BAD_MAPPING_REF;
            } else if (strcmp(fault_case, "stale-mapping") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_STALE_MAPPING;
            } else if (strcmp(fault_case, "released-import") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_RELEASED_IMPORT;
            } else if (strcmp(fault_case, "wrong-requester") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_WRONG_REQUESTER;
            } else if (strcmp(fault_case, "oob") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_OOB;
            } else if (strcmp(fault_case, "address-overflow") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_ADDRESS_OVERFLOW;
            } else if (strcmp(fault_case,
                              "role-access-mismatch") == 0) {
                config->fault_case =
                    PTO_DIRECT_FAULT_ROLE_ACCESS_MISMATCH;
            } else if (strcmp(fault_case, "tstore-on-read") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_TSTORE_ON_READ;
            } else if (strcmp(fault_case, "tload-on-write") == 0) {
                config->fault_case = PTO_DIRECT_FAULT_TLOAD_ON_WRITE;
            } else {
                fprintf(stderr, "invalid fault case: %s\n", fault_case);
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
        } else if (strcmp(option, "--cancel-after-ms") == 0) {
            config->cancel_after_ms = value;
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
        (config->expectation == PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED &&
         (config->cancel_after_ms == 0 ||
          config->cancel_after_ms >= config->timeout_ms)) ||
        (config->expectation != PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED &&
         config->cancel_after_ms != 0) ||
        (config->fault_case == PTO_DIRECT_FAULT_NONE &&
         (config->expectation == PTO_DIRECT_EXPECT_BAD_MEMREF ||
          config->expectation == PTO_DIRECT_EXPECT_ACCESS_DENIED)) ||
        (config->fault_case != PTO_DIRECT_FAULT_NONE &&
         config->expectation != fault_case_expectation(config->fault_case)) ||
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

static bool output_is_sentinel(const void *address,
                               const struct pto_direct_layout *layout,
                               uint32_t elements,
                               uint32_t *mismatch_index,
                               uint32_t *actual_bits)
{
    const volatile uint32_t *output =
        (const volatile uint32_t *)((const uint8_t *)address +
                                    layout->output_offset);
    uint32_t index;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    for (index = 0; index < elements; index++) {
        if (output[index] != PTO_DIRECT_OUTPUT_SENTINEL) {
            if (mismatch_index) {
                *mismatch_index = index;
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
            if (config->expectation != PTO_DIRECT_EXPECT_SUCCESS) {
                fprintf(stderr,
                        "LINGQU_SHMEM_PTO_RESULT role=producer status=fail "
                        "reason=unexpected_success "
                        "expected=%s\n",
                        expectation_name(config->expectation));
                goto out;
            }
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
        if (config->expectation != PTO_DIRECT_EXPECT_SUCCESS) {
            uint32_t changed_index = 0;
            uint32_t changed_bits = PTO_DIRECT_OUTPUT_SENTINEL;

            if (output_is_sentinel(region.addr, layout, config->elements,
                                   &changed_index, &changed_bits)) {
                printf("LINGQU_SHMEM_PTO_RESULT role=producer status=pass "
                       "expected=%s "
                       "observed=verify_timeout output_unchanged=1 "
                       "elements=%u sentinel=0x%08x\n",
                       expectation_name(config->expectation),
                       config->elements, PTO_DIRECT_OUTPUT_SENTINEL);
                rc = 0;
                goto out;
            }
            fprintf(stderr,
                    "LINGQU_SHMEM_PTO_RESULT role=producer status=fail "
                    "reason=output_changed_after_expected_failure "
                    "expected=%s index=%u actual=0x%08x\n",
                    expectation_name(config->expectation), changed_index,
                    changed_bits);
            goto out;
        }
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

static uint32_t pto_direct_crc32_ieee_update(uint32_t crc,
                                             const uint8_t *bytes,
                                             size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        uint32_t value = crc ^ bytes[index];
        uint32_t bit;

        for (bit = 0; bit < 8; bit++) {
            value = (value >> 1) ^
                    (UINT32_C(0xedb88320) &
                     (0u - (value & UINT32_C(1))));
        }
        crc = value;
    }
    return crc;
}

static bool metadata_span(uint64_t metadata_iova,
                          size_t metadata_capacity,
                          uint64_t span_iova,
                          size_t span_bytes,
                          size_t *offset_out)
{
    uint64_t offset;

    if (!offset_out || span_iova < metadata_iova) {
        return false;
    }
    offset = span_iova - metadata_iova;
    if (offset > metadata_capacity ||
        span_bytes > metadata_capacity - (size_t)offset) {
        return false;
    }
    *offset_out = (size_t)offset;
    return true;
}

static int refresh_fault_metadata_crc(
    uint8_t *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    struct lingqu_shmem_pto_wire_result *wire_result)
{
    LingquPtoDispatchControlV2 *control;
    LingquShmemMemrefV1 *wire_memrefs;
    size_t memref_offset;
    size_t memref_bytes;
    uint32_t crc = UINT32_C(0xffffffff);
    uint32_t index;

    if (!metadata || !wire_result ||
        metadata_capacity < sizeof(*control)) {
        return -EINVAL;
    }
    control = (LingquPtoDispatchControlV2 *)metadata;
    if (control->memref_count != 3 || control->scalar_count != 0) {
        return -EPROTO;
    }
    memref_bytes = (size_t)control->memref_count *
                   sizeof(*wire_memrefs);
    if (!metadata_span(metadata_iova, metadata_capacity,
                       control->memref_table_iova, memref_bytes,
                       &memref_offset)) {
        return -EPROTO;
    }
    wire_memrefs = (LingquShmemMemrefV1 *)(metadata + memref_offset);

    control->metadata_crc32 = 0;
    crc = pto_direct_crc32_ieee_update(
        crc, metadata, sizeof(*control));
    crc = pto_direct_crc32_ieee_update(
        crc, (const uint8_t *)wire_memrefs, memref_bytes);
    for (index = 0; index < control->memref_count; index++) {
        const LingquShmemMemrefV1 *memref = &wire_memrefs[index];
        size_t shape_offset;
        size_t stride_offset;
        size_t view_bytes;

        if (memref->rank == 0 || memref->rank > LINGQU_PTO_MAX_RANK) {
            return -EPROTO;
        }
        view_bytes = (size_t)memref->rank * sizeof(uint32_t);
        if (!metadata_span(metadata_iova, metadata_capacity,
                           memref->shape_table_iova, view_bytes,
                           &shape_offset) ||
            !metadata_span(metadata_iova, metadata_capacity,
                           memref->stride_table_iova, view_bytes,
                           &stride_offset)) {
            return -EPROTO;
        }
        crc = pto_direct_crc32_ieee_update(
            crc, metadata + shape_offset, view_bytes);
        crc = pto_direct_crc32_ieee_update(
            crc, metadata + stride_offset, view_bytes);
    }
    control->metadata_crc32 = crc ^ UINT32_C(0xffffffff);
    wire_result->metadata_crc32 = control->metadata_crc32;
    return 0;
}

static int inject_fault_case(
    const struct pto_direct_config *config,
    uint8_t *metadata,
    size_t metadata_capacity,
    uint64_t metadata_iova,
    uint64_t mapping_bytes,
    struct lingqu_shmem_pto_wire_result *wire_result,
    struct obmm_async *async_runtime,
    struct obmm_async_map *async_map,
    int obmm_fd,
    uint64_t *import_mem_id,
    struct obmm_helpers_region *imported)
{
    LingquPtoDispatchControlV2 *control;
    LingquShmemMemrefV1 *wire_memrefs;
    size_t memref_offset;
    int rc;

    if (!config || config->fault_case == PTO_DIRECT_FAULT_NONE) {
        return 0;
    }
    if (!metadata || !wire_result || !async_runtime || !async_map ||
        obmm_fd < 0 || !import_mem_id || !imported) {
        return -EINVAL;
    }
    control = (LingquPtoDispatchControlV2 *)metadata;
    if (control->memref_count != 3 ||
        !metadata_span(metadata_iova, metadata_capacity,
                       control->memref_table_iova,
                       3 * sizeof(*wire_memrefs), &memref_offset)) {
        return -EPROTO;
    }
    wire_memrefs = (LingquShmemMemrefV1 *)(metadata + memref_offset);

    if (config->fault_case == PTO_DIRECT_FAULT_TSTORE_ON_READ ||
        config->fault_case == PTO_DIRECT_FAULT_TLOAD_ON_WRITE) {
        printf("LINGQU_SHMEM_PTO role=consumer stage=fault_selected "
               "fault=%s source=callable-artifact expected=%s "
               "metadata_crc32=0x%08x map_active=1\n",
               fault_case_name(config->fault_case),
               expectation_name(config->expectation),
               wire_result->metadata_crc32);
        return 0;
    }

    if (config->fault_case == PTO_DIRECT_FAULT_STALE_MAPPING) {
        uint64_t map_id = async_map->id;
        uint64_t map_generation = async_map->generation;

        rc = obmm_async_map_unregister(async_runtime, async_map);
        if (rc != 0) {
            return rc;
        }
        printf("LINGQU_SHMEM_PTO role=consumer stage=fault_injected "
               "fault=%s expected=%s map_id=%" PRIu64
               " map_generation=%" PRIu64 " map_active=0\n",
               fault_case_name(config->fault_case),
               expectation_name(config->expectation), map_id,
               map_generation);
        return 0;
    }

    if (config->fault_case == PTO_DIRECT_FAULT_RELEASED_IMPORT) {
        uint64_t released_mem_id = *import_mem_id;

        if (released_mem_id == 0) {
            return -EINVAL;
        }
        obmm_unmap_region(imported);
        rc = obmm_do_unimport(obmm_fd, released_mem_id);
        if (rc != 0) {
            return errno != 0 ? -errno : rc;
        }
        *import_mem_id = 0;
        printf("LINGQU_SHMEM_PTO role=consumer stage=fault_injected "
               "fault=%s expected=%s import_mem_id=%" PRIu64
               " import_active=0 map_id=%" PRIu64
               " map_generation=%" PRIu64 " map_active=1\n",
               fault_case_name(config->fault_case),
               expectation_name(config->expectation), released_mem_id,
               async_map->id, async_map->generation);
        return 0;
    }

    switch (config->fault_case) {
    case PTO_DIRECT_FAULT_BAD_MAPPING_REF: {
        uint64_t mapping_ref = wire_memrefs[0].opaque_mapping_ref;
        uint64_t map_id = lingqu_pto_obmm_mapping_ref_map_id(mapping_ref);
        uint64_t generation =
            lingqu_pto_obmm_mapping_ref_generation(mapping_ref);
        uint64_t bad_generation =
            generation == LINGQU_PTO_OBMM_MAP_GENERATION_MAX ?
            generation - 1 : generation + 1;

        wire_memrefs[0].opaque_mapping_ref =
            lingqu_pto_obmm_mapping_ref_encode(map_id, bad_generation);
        if (wire_memrefs[0].opaque_mapping_ref == 0 ||
            wire_memrefs[0].opaque_mapping_ref == mapping_ref) {
            return -ERANGE;
        }
        break;
    }
    case PTO_DIRECT_FAULT_WRONG_REQUESTER:
        control->requester_cna = config->requester_cna == 1 ? 2 : 1;
        break;
    case PTO_DIRECT_FAULT_OOB:
        if (wire_memrefs[2].byte_length >= mapping_bytes) {
            return -ERANGE;
        }
        wire_memrefs[2].byte_offset =
            mapping_bytes - wire_memrefs[2].byte_length + 1;
        break;
    case PTO_DIRECT_FAULT_ADDRESS_OVERFLOW:
        wire_memrefs[0].ub_gm_addr =
            UINT64_MAX - wire_memrefs[0].byte_length + 1;
        break;
    case PTO_DIRECT_FAULT_ROLE_ACCESS_MISMATCH:
        wire_memrefs[2].access = LINGQU_PTO_UB_GM_READ;
        break;
    case PTO_DIRECT_FAULT_TSTORE_ON_READ:
    case PTO_DIRECT_FAULT_TLOAD_ON_WRITE:
    case PTO_DIRECT_FAULT_NONE:
    case PTO_DIRECT_FAULT_STALE_MAPPING:
    case PTO_DIRECT_FAULT_RELEASED_IMPORT:
        return -EINVAL;
    }
    rc = refresh_fault_metadata_crc(
        metadata, metadata_capacity, metadata_iova, wire_result);
    if (rc != 0) {
        return rc;
    }
    printf("LINGQU_SHMEM_PTO role=consumer stage=fault_injected "
           "fault=%s expected=%s metadata_crc32=0x%08x map_active=1\n",
           fault_case_name(config->fault_case),
           expectation_name(config->expectation),
           wire_result->metadata_crc32);
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
    submit_rc = obmm_do_import(
        obmm_fd, &meta, local_cna, local_pas[0], config->token_value,
        &import_mem_id);
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
    metadata = mmap(NULL, PTO_DIRECT_METADATA_BYTES,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (metadata == MAP_FAILED) {
        goto out;
    }
    memset(metadata, 0, PTO_DIRECT_METADATA_BYTES);
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
            &request, metadata, PTO_DIRECT_METADATA_BYTES, metadata_iova,
            &slot,
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
    submit_rc = inject_fault_case(
        config, metadata, PTO_DIRECT_METADATA_BYTES, metadata_iova,
        meta.size, &wire_result, async_runtime, &async_map, obmm_fd,
        &import_mem_id, &imported);
    if (submit_rc != 0) {
        fprintf(stderr,
                "[lingqu_shmem_pto] consumer fault_injection "
                "fault=%s error=%d\n",
                fault_case_name(config->fault_case), submit_rc);
        goto out;
    }
    if (config->expectation == PTO_DIRECT_EXPECT_AUTHORIZATION_CANCELLED) {
        submit_rc = lingqu_shmem_pto_endpoint_submit_cancel_after(
            endpoint, &slot, config->timeout_ms, config->cancel_after_ms,
            &completion);
    } else {
        submit_rc = lingqu_shmem_pto_endpoint_submit(
            endpoint, &slot, config->timeout_ms, &completion);
    }
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
        const char *expected_error =
            expectation_error_code(config->expectation);

        if (expected_error != NULL &&
            completion.status == PTO_DIRECT_COMPLETION_FAILED &&
            strcmp(completion.error_code, expected_error) == 0) {
            printf("LINGQU_SHMEM_PTO_RESULT role=consumer status=pass "
                   "expected=%s "
                   "observed=completion error=%s\n",
                   expectation_name(config->expectation),
                   completion.error_code);
            rc = 0;
            goto out;
        }
        fprintf(stderr,
                "LINGQU_SHMEM_PTO_RESULT role=consumer status=fail "
                "reason=completion error=%s\n",
                completion.error_code[0] ? completion.error_code : "unknown");
        goto out;
    }
    if (config->expectation != PTO_DIRECT_EXPECT_SUCCESS) {
        fprintf(stderr,
                "LINGQU_SHMEM_PTO_RESULT role=consumer status=fail "
                "reason=unexpected_success expected=%s\n",
                expectation_name(config->expectation));
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
        munmap(metadata, PTO_DIRECT_METADATA_BYTES);
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
           "local_cna=0x%x elements=%u generation=%" PRIu64
           " expected=%s fault_case=%s cancel_after_ms=%" PRIu64 "\n",
           config.role == PTO_DIRECT_ROLE_PRODUCER ? "producer" :
                                                     "consumer",
           config.node_id, config.node_count, local_cna,
           config.elements, config.generation,
           expectation_name(config.expectation),
           fault_case_name(config.fault_case), config.cancel_after_ms);
    if (config.role == PTO_DIRECT_ROLE_PRODUCER) {
        return run_producer(&config, &layout, local_cna);
    }
    return run_consumer(&config, &layout, local_cna);
}
