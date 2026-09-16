/* SPDX-License-Identifier: MIT */
#include "lingqu_shmem_pto_cpu_probe.h"

#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>

int main(void)
{
    long page = sysconf(_SC_PAGESIZE);
    assert(page > 0);
    uint8_t *memory = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(memory != MAP_FAILED);
    memory[0] = 42;
    struct sigaction segv_before, bus_before, after;
    assert(sigaction(SIGSEGV, NULL, &segv_before) == 0);
    assert(sigaction(SIGBUS, NULL, &bus_before) == 0);
    assert(mprotect(memory, (size_t)page, PROT_READ) == 0);
    struct lingqu_shmem_pto_cpu_fault fault;
    for (unsigned i = 0; i < 2; ++i) {
        assert(lingqu_shmem_pto_probe_readonly(memory, &fault) == 0);
        assert(fault.address == (uintptr_t)memory && fault.code > 0);
        assert(fault.signal == SIGSEGV || fault.signal == SIGBUS);
        assert(memory[0] == 42);
    }
    assert(sigaction(SIGSEGV, NULL, &after) == 0);
    assert(after.sa_sigaction == segv_before.sa_sigaction);
    assert(sigaction(SIGBUS, NULL, &after) == 0);
    assert(after.sa_sigaction == bus_before.sa_sigaction);
    assert(mprotect(memory, (size_t)page, PROT_NONE) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        assert(lingqu_shmem_pto_probe_revoked_read(memory, &fault) == 0);
        assert(fault.address == (uintptr_t)memory && fault.code > 0);
        assert(fault.signal == SIGSEGV || fault.signal == SIGBUS);
    }
    assert(sigaction(SIGSEGV, NULL, &after) == 0);
    assert(after.sa_sigaction == segv_before.sa_sigaction);
    assert(sigaction(SIGBUS, NULL, &after) == 0);
    assert(after.sa_sigaction == bus_before.sa_sigaction);
    assert(mprotect(memory, (size_t)page, PROT_READ | PROT_WRITE) == 0);
    assert(lingqu_shmem_pto_probe_revoked_read(memory, &fault) == -EPROTO);
    assert(lingqu_shmem_pto_probe_readonly(memory, &fault) == -EPROTO);
    assert(memory[0] == 43 && fault.address == 0 && fault.signal == 0);
    assert(lingqu_shmem_pto_probe_revoked_read(NULL, &fault) == -EINVAL);
    assert(lingqu_shmem_pto_probe_revoked_read(memory, NULL) == -EINVAL);
    assert(lingqu_shmem_pto_probe_readonly(NULL, &fault) == -EINVAL);
    assert(lingqu_shmem_pto_probe_readonly(memory, NULL) == -EINVAL);
    assert(munmap(memory, (size_t)page) == 0);
    puts("pto cpu readonly probe golden: pass");
    return 0;
}
