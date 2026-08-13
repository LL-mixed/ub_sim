/* SPDX-License-Identifier: MIT */
#ifndef OBMM_UFFD_H
#define OBMM_UFFD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct obmm_uffd {
    int fd;
    void *range_start;
    size_t range_length;
    bool registered;
    bool poison_supported;
};

struct obmm_uffd_fault {
    uint64_t address;
    uint64_t flags;
};

int obmm_uffd_open(struct obmm_uffd *uffd);
int obmm_uffd_register_missing(struct obmm_uffd *uffd,
                               void *range_start, size_t range_length);
int obmm_uffd_read_fault(struct obmm_uffd *uffd,
                         struct obmm_uffd_fault *fault);
int obmm_uffd_copy(struct obmm_uffd *uffd, void *destination,
                   const void *source, size_t length);
int obmm_uffd_poison(struct obmm_uffd *uffd, void *destination,
                     size_t length);
int obmm_uffd_unregister(struct obmm_uffd *uffd);
void obmm_uffd_close(struct obmm_uffd *uffd);

#endif
