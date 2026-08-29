/* SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lingqu_shmem_pto_endpoint.h"

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "check failed line=%d: %s\n", __LINE__,       \
                    #condition);                                             \
            return 1;                                                        \
        }                                                                    \
    } while (0)

static void store_u64_le(uint8_t bytes[8], uint64_t value)
{
    size_t index;

    for (index = 0; index < sizeof(value); index++) {
        bytes[index] = (uint8_t)(value >> (index * 8));
    }
}

int main(void)
{
    struct lingqu_shmem_pto_completion completion;
    uint8_t slot[LINGQU_SHMEM_PTO_COMPLETION_BYTES] = { 0 };
    const char failure[] = "pto_ub_gm_bad_memref";

    store_u64_le(slot, UINT64_C(0x1122334455667788));
    slot[8] = 0;
    slot[9] = 1;
    slot[10] = 1;
    store_u64_le(slot + 11, UINT64_C(0x0102030405060708));
    CHECK(lingqu_shmem_pto_completion_decode(slot, &completion) == 0);
    CHECK(completion.op_id == UINT64_C(0x1122334455667788));
    CHECK(completion.finished_at == UINT64_C(0x0102030405060708));
    CHECK(completion.error_code[0] == '\0');
    CHECK(lingqu_shmem_pto_completion_succeeded(&completion));

    memset(slot, 0, sizeof(slot));
    store_u64_le(slot, UINT64_C(0x8877665544332211));
    slot[9] = 1;
    slot[10] = 3;
    slot[11] = sizeof(failure) - 1;
    memcpy(slot + 12, failure, sizeof(failure) - 1);
    store_u64_le(slot + 12 + sizeof(failure) - 1,
                 UINT64_C(0xa1a2a3a4a5a6a7a8));
    CHECK(lingqu_shmem_pto_completion_decode(slot, &completion) == 0);
    CHECK(completion.status == 3);
    CHECK(strcmp(completion.error_code, failure) == 0);
    CHECK(completion.finished_at == UINT64_C(0xa1a2a3a4a5a6a7a8));
    CHECK(!lingqu_shmem_pto_completion_succeeded(&completion));

    slot[11] = LINGQU_SHMEM_PTO_COMPLETION_CODE_BYTES;
    CHECK(lingqu_shmem_pto_completion_decode(slot, &completion) == -EPROTO);
    slot[11] = 0;
    CHECK(lingqu_shmem_pto_completion_decode(slot, &completion) == -EPROTO);
    slot[11] = sizeof(failure) - 1;
    slot[9] = 0;
    CHECK(lingqu_shmem_pto_completion_decode(slot, &completion) == -EPROTO);

    puts("lingqu_shmem_pto_endpoint_golden=pass");
    return 0;
}
