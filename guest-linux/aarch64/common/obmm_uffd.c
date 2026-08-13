/* SPDX-License-Identifier: MIT */
#define _GNU_SOURCE

#include "obmm_uffd.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

/* Linux 6.6 standard UAPI compatibility for older cross sysroots. */
#ifndef UFFDIO_POISON
#define _UFFDIO_POISON 0x08
struct uffdio_poison {
    struct uffdio_range range;
    uint64_t mode;
    int64_t updated;
};
#define UFFDIO_POISON _IOWR(UFFDIO, _UFFDIO_POISON, \
                            struct uffdio_poison)
#endif

static int obmm_uffd_neg_errno(void)
{
    return errno ? -errno : -EIO;
}

int obmm_uffd_open(struct obmm_uffd *uffd)
{
    struct uffdio_api api = {
        .api = UFFD_API,
    };
    int fd;
    int ret;

    if (!uffd) {
        return -EINVAL;
    }
    memset(uffd, 0, sizeof(*uffd));
    uffd->fd = -1;
    fd = syscall(__NR_userfaultfd,
                 UFFD_USER_MODE_ONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return errno == ENOSYS || errno == EPERM || errno == EINVAL ?
            -EOPNOTSUPP : obmm_uffd_neg_errno();
    }
    ret = ioctl(fd, UFFDIO_API, &api);
    if (ret != 0 || api.api != UFFD_API) {
        int error = ret == 0 || errno == ENOTTY || errno == EINVAL ?
            -EOPNOTSUPP : obmm_uffd_neg_errno();

        close(fd);
        return error;
    }
    uffd->fd = fd;
    return 0;
}

int obmm_uffd_register_missing(struct obmm_uffd *uffd,
                               void *range_start, size_t range_length)
{
    struct uffdio_register registration;
    uintptr_t start = (uintptr_t)range_start;
    long page_size = sysconf(_SC_PAGESIZE);

    if (!uffd || uffd->fd < 0 || !range_start || !range_length ||
        uffd->registered || page_size <= 0 ||
        start % (uintptr_t)page_size ||
        range_length % (size_t)page_size ||
        start + range_length < start) {
        return -EINVAL;
    }
    memset(&registration, 0, sizeof(registration));
    registration.range.start = (uintptr_t)range_start;
    registration.range.len = range_length;
    registration.mode = UFFDIO_REGISTER_MODE_MISSING;
    if (ioctl(uffd->fd, UFFDIO_REGISTER, &registration) != 0) {
        return errno == ENOTTY || errno == EINVAL || errno == EPERM ?
            -EOPNOTSUPP : obmm_uffd_neg_errno();
    }
    if (!(registration.ioctls & (1ULL << _UFFDIO_COPY))) {
        struct uffdio_range range = registration.range;

        ioctl(uffd->fd, UFFDIO_UNREGISTER, &range);
        return -EOPNOTSUPP;
    }
    uffd->range_start = range_start;
    uffd->range_length = range_length;
    uffd->registered = true;
    uffd->poison_supported =
        registration.ioctls & (1ULL << _UFFDIO_POISON);
    return 0;
}

int obmm_uffd_read_fault(struct obmm_uffd *uffd,
                         struct obmm_uffd_fault *fault)
{
    struct uffd_msg message;
    ssize_t bytes;

    if (!uffd || uffd->fd < 0 || !fault) {
        return -EINVAL;
    }
    bytes = read(uffd->fd, &message, sizeof(message));
    if (bytes < 0) {
        return errno == EAGAIN || errno == EINTR ? -EAGAIN :
            obmm_uffd_neg_errno();
    }
    if (bytes != sizeof(message)) {
        return -EPROTO;
    }
    if (message.event != UFFD_EVENT_PAGEFAULT) {
        return -EPROTO;
    }
    fault->address = message.arg.pagefault.address;
    fault->flags = message.arg.pagefault.flags;
    return 0;
}

int obmm_uffd_copy(struct obmm_uffd *uffd, void *destination,
                   const void *source, size_t length)
{
    struct uffdio_copy copy;
    uintptr_t destination_address = (uintptr_t)destination;
    uintptr_t source_address = (uintptr_t)source;
    long page_size = sysconf(_SC_PAGESIZE);

    if (!uffd || !uffd->registered || !destination || !source || !length ||
        page_size <= 0 || destination_address % (uintptr_t)page_size ||
        source_address % (uintptr_t)page_size ||
        length % (size_t)page_size) {
        return -EINVAL;
    }
    memset(&copy, 0, sizeof(copy));
    copy.src = (uintptr_t)source;
    copy.dst = (uintptr_t)destination;
    copy.len = length;
    if (ioctl(uffd->fd, UFFDIO_COPY, &copy) != 0) {
        return obmm_uffd_neg_errno();
    }
    return copy.copy == (int64_t)length ? 0 : -EIO;
}

int obmm_uffd_poison(struct obmm_uffd *uffd, void *destination,
                     size_t length)
{
    struct uffdio_poison poison;

    if (!uffd || !uffd->registered || !uffd->poison_supported ||
        !destination || !length) {
        return -EOPNOTSUPP;
    }
    memset(&poison, 0, sizeof(poison));
    poison.range.start = (uintptr_t)destination;
    poison.range.len = length;
    if (ioctl(uffd->fd, UFFDIO_POISON, &poison) != 0) {
        return obmm_uffd_neg_errno();
    }
    return poison.updated == (int64_t)length ? 0 : -EIO;
}

int obmm_uffd_unregister(struct obmm_uffd *uffd)
{
    struct uffdio_range range;

    if (!uffd || uffd->fd < 0) {
        return -EINVAL;
    }
    if (!uffd->registered) {
        return 0;
    }
    range.start = (uintptr_t)uffd->range_start;
    range.len = uffd->range_length;
    if (ioctl(uffd->fd, UFFDIO_UNREGISTER, &range) != 0) {
        return obmm_uffd_neg_errno();
    }
    uffd->registered = false;
    uffd->range_start = NULL;
    uffd->range_length = 0;
    return 0;
}

void obmm_uffd_close(struct obmm_uffd *uffd)
{
    if (!uffd) {
        return;
    }
    if (uffd->registered) {
        obmm_uffd_unregister(uffd);
    }
    if (uffd->fd >= 0) {
        close(uffd->fd);
    }
    memset(uffd, 0, sizeof(*uffd));
    uffd->fd = -1;
}
