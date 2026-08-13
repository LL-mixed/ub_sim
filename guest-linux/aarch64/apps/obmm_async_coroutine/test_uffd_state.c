/* SPDX-License-Identifier: MIT */
#include "uffd_state.h"

#include <assert.h>

int main(void)
{
    struct obmm_uffd_page_record record;

    obmm_uffd_page_reset(&record, 7);
    assert(obmm_uffd_page_claim(&record, 6) == OBMM_UFFD_FAULT_STALE);
    assert(obmm_uffd_page_claim(&record, 7) == OBMM_UFFD_FAULT_OWNER);
    assert(obmm_uffd_page_claim(&record, 7) == OBMM_UFFD_FAULT_DUPLICATE);
    assert(record.waiters == 2);
    assert(obmm_uffd_page_remote_begin(&record, 7));
    assert(record.remote_reads == 1);
    assert(obmm_uffd_page_remote_done(&record, 7, 0x1234));
    assert(obmm_uffd_page_copy_begin(&record, 7));
    assert(!obmm_uffd_page_resolve(&record, 7, true, 0x5678));
    assert(obmm_uffd_page_resolve(&record, 7, true, 0x1234));

    obmm_uffd_page_reset(&record, 8);
    assert(obmm_uffd_page_claim(&record, 8) == OBMM_UFFD_FAULT_OWNER);
    assert(obmm_uffd_page_remote_begin(&record, 8));
    assert(obmm_uffd_page_fail(&record, 8, true));
    assert(record.state == OBMM_UFFD_PAGE_POISONED);
    assert(!obmm_uffd_page_remote_done(&record, 8, 0));
    return 0;
}
