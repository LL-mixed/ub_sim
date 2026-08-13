/* SPDX-License-Identifier: MIT */
#include "uffd_state.h"

#include <string.h>

void obmm_uffd_page_reset(struct obmm_uffd_page_record *record,
                          uint64_t generation)
{
    memset(record, 0, sizeof(*record));
    record->generation = generation;
}

enum obmm_uffd_fault_claim obmm_uffd_page_claim(
    struct obmm_uffd_page_record *record, uint64_t generation)
{
    if (!record || !generation || record->generation != generation) {
        return OBMM_UFFD_FAULT_STALE;
    }
    if (record->state == OBMM_UFFD_PAGE_EMPTY) {
        record->state = OBMM_UFFD_PAGE_FAULT_RECEIVED;
        record->waiters = 1;
        return OBMM_UFFD_FAULT_OWNER;
    }
    if (record->state == OBMM_UFFD_PAGE_FAULT_RECEIVED ||
        record->state == OBMM_UFFD_PAGE_READING_REMOTE ||
        record->state == OBMM_UFFD_PAGE_COPY_READY ||
        record->state == OBMM_UFFD_PAGE_COPYING ||
        record->state == OBMM_UFFD_PAGE_RESOLVED) {
        record->waiters++;
        return OBMM_UFFD_FAULT_DUPLICATE;
    }
    return OBMM_UFFD_FAULT_STALE;
}

bool obmm_uffd_page_remote_begin(struct obmm_uffd_page_record *record,
                                 uint64_t generation)
{
    if (!record || record->generation != generation ||
        record->state != OBMM_UFFD_PAGE_FAULT_RECEIVED) {
        return false;
    }
    record->state = OBMM_UFFD_PAGE_READING_REMOTE;
    record->remote_reads++;
    return true;
}

bool obmm_uffd_page_remote_done(struct obmm_uffd_page_record *record,
                                uint64_t generation, uint64_t checksum)
{
    if (!record || record->generation != generation ||
        record->state != OBMM_UFFD_PAGE_READING_REMOTE) {
        return false;
    }
    record->checksum = checksum;
    record->state = OBMM_UFFD_PAGE_COPY_READY;
    return true;
}

bool obmm_uffd_page_copy_begin(struct obmm_uffd_page_record *record,
                               uint64_t generation)
{
    if (!record || record->generation != generation ||
        record->state != OBMM_UFFD_PAGE_COPY_READY) {
        return false;
    }
    record->state = OBMM_UFFD_PAGE_COPYING;
    return true;
}

bool obmm_uffd_page_resolve(struct obmm_uffd_page_record *record,
                            uint64_t generation, bool already_present,
                            uint64_t checksum)
{
    if (!record || record->generation != generation ||
        record->state != OBMM_UFFD_PAGE_COPYING ||
        (already_present && checksum != record->checksum)) {
        return false;
    }
    record->state = OBMM_UFFD_PAGE_RESOLVED;
    return true;
}

bool obmm_uffd_page_fail(struct obmm_uffd_page_record *record,
                         uint64_t generation, bool poisoned)
{
    if (!record || record->generation != generation ||
        (record->state != OBMM_UFFD_PAGE_FAULT_RECEIVED &&
         record->state != OBMM_UFFD_PAGE_READING_REMOTE &&
         record->state != OBMM_UFFD_PAGE_COPY_READY &&
         record->state != OBMM_UFFD_PAGE_COPYING)) {
        return false;
    }
    record->state = poisoned ? OBMM_UFFD_PAGE_POISONED :
        OBMM_UFFD_PAGE_FAIL_STOP;
    return true;
}
