#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_finit_module
#define SYS_finit_module 313
#endif

int main(int argc, char **argv)
{
    int fd;
    int rc;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <module.ko>\n", argv[0]);
        return 2;
    }

    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "open(%s) failed: %s\n", argv[1], strerror(errno));
        return 1;
    }

    rc = syscall(SYS_finit_module, fd, "", 0);
    close(fd);
    if (rc != 0) {
        fprintf(stderr, "finit_module(%s) failed: %s\n", argv[1], strerror(errno));
        return 1;
    }

    return 0;
}
