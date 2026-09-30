/* SPDX-License-Identifier: MIT */
#ifndef DEEPSEEK_V4_FLASH_HISTORY_GUEST_H
#define DEEPSEEK_V4_FLASH_HISTORY_GUEST_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define DEEPSEEK_HISTORY_MAGIC UINT64_C(0x4453344849535446)
#define DEEPSEEK_HISTORY_HEADER_BYTES 64U

struct deepseek_v4_flash_history_header {
    uint32_t past;
    uint32_t token;
    uint32_t first;
    uint32_t end;
    uint64_t bytes;
    uint64_t checksum;
};

static inline uint64_t deepseek_history_u64(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i)
        value |= (uint64_t)bytes[i] << (8U * i);
    return value;
}

/* Leaves the stream at the state payload; the caller retains file ownership. */
static inline int deepseek_v4_flash_history_read(
    FILE *file, struct deepseek_v4_flash_history_header *header)
{
    unsigned char bytes[DEEPSEEK_HISTORY_HEADER_BYTES];
    uint64_t words[8];
    long length;

    if (!file || !header || fseek(file, 0, SEEK_SET) != 0 ||
        fread(bytes, 1, sizeof(bytes), file) != sizeof(bytes))
        return -EINVAL;
    for (unsigned int i = 0; i < 8; ++i)
        words[i] = deepseek_history_u64(bytes + i * 8U);
    if (words[0] != DEEPSEEK_HISTORY_MAGIC || words[1] != 1 ||
        !words[2] || words[2] >= UINT32_MAX || words[3] >= 129280 ||
        words[4] >= words[5] || words[5] > 43 ||
        words[6] < 32 || words[6] > UINT32_MAX)
        return -EPROTO;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        (uint64_t)length != sizeof(bytes) + words[6] ||
        fseek(file, sizeof(bytes), SEEK_SET) != 0)
        return -EPROTO;
    *header = (struct deepseek_v4_flash_history_header) {
        .past = (uint32_t)words[2], .token = (uint32_t)words[3],
        .first = (uint32_t)words[4], .end = (uint32_t)words[5],
        .bytes = words[6], .checksum = words[7],
    };
    return 0;
}

#endif
