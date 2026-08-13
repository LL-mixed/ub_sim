/* SPDX-License-Identifier: MIT */
#ifndef OBMM_ASYNC_UFFD_STATE_H
#define OBMM_ASYNC_UFFD_STATE_H

#include <stdbool.h>
#include <stdint.h>

enum obmm_uffd_page_state {
    OBMM_UFFD_PAGE_EMPTY,
    OBMM_UFFD_PAGE_FAULT_RECEIVED,
    OBMM_UFFD_PAGE_READING_REMOTE,
    OBMM_UFFD_PAGE_COPY_READY,
    OBMM_UFFD_PAGE_COPYING,
    OBMM_UFFD_PAGE_RESOLVED,
    OBMM_UFFD_PAGE_READ_FAILED,
    OBMM_UFFD_PAGE_POISONED,
    OBMM_UFFD_PAGE_FAIL_STOP,
};

enum obmm_uffd_fault_claim {
    OBMM_UFFD_FAULT_OWNER,
    OBMM_UFFD_FAULT_DUPLICATE,
    OBMM_UFFD_FAULT_STALE,
};

struct obmm_uffd_page_record {
    uint64_t generation;
    enum obmm_uffd_page_state state;
    uint32_t waiters;
    uint32_t remote_reads;
    uint64_t checksum;
};

void obmm_uffd_page_reset(struct obmm_uffd_page_record *record,
                          uint64_t generation);
enum obmm_uffd_fault_claim obmm_uffd_page_claim(
    struct obmm_uffd_page_record *record, uint64_t generation);
bool obmm_uffd_page_remote_begin(struct obmm_uffd_page_record *record,
                                 uint64_t generation);
bool obmm_uffd_page_remote_done(struct obmm_uffd_page_record *record,
                                uint64_t generation, uint64_t checksum);
bool obmm_uffd_page_copy_begin(struct obmm_uffd_page_record *record,
                               uint64_t generation);
bool obmm_uffd_page_resolve(struct obmm_uffd_page_record *record,
                            uint64_t generation, bool already_present,
                            uint64_t checksum);
bool obmm_uffd_page_fail(struct obmm_uffd_page_record *record,
                         uint64_t generation, bool poisoned);

#endif
