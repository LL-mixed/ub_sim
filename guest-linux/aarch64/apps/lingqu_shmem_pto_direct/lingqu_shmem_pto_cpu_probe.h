/* SPDX-License-Identifier: MIT */
#ifndef LINGQU_SHMEM_PTO_CPU_PROBE_H
#define LINGQU_SHMEM_PTO_CPU_PROBE_H

#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

struct lingqu_shmem_pto_cpu_fault {
    uintptr_t address;
    int signal, code;
};

static sigjmp_buf pto_cpu_probe_jump;
static volatile sig_atomic_t pto_cpu_probe_armed;
static volatile sig_atomic_t pto_cpu_probe_signal, pto_cpu_probe_code;
static volatile uintptr_t pto_cpu_probe_address;

static void pto_cpu_probe_handler(int signal, siginfo_t *info, void *context)
{
    (void)context;
    if (pto_cpu_probe_armed && info && info->si_code > 0 &&
        (uintptr_t)info->si_addr == pto_cpu_probe_address) {
        pto_cpu_probe_signal = signal;
        pto_cpu_probe_code = info->si_code;
        siglongjmp(pto_cpu_probe_jump, 1);
    }
    _exit(128 + signal);
}

/* Call only before starting threads and with a retained, readable CPU pin.
 * OBMM VMAs are DONTCOPY: the probe must stay in the mapping-owning process. */
static int lingqu_shmem_pto_probe_readonly(uint8_t *address,
                                          struct lingqu_shmem_pto_cpu_fault *fault)
{
    struct sigaction action = {0}, old_segv, old_bus;
    volatile sig_atomic_t observed = 0;
    if (!address || !fault) return -EINVAL;
    if (pto_cpu_probe_armed) return -EBUSY;
    *fault = (struct lingqu_shmem_pto_cpu_fault) {0};
    volatile uint8_t original = *(volatile uint8_t *)address;
    action.sa_sigaction = pto_cpu_probe_handler;
    action.sa_flags = SA_SIGINFO;
    sigfillset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &old_segv)) return -errno;
    if (sigaction(SIGBUS, &action, &old_bus)) {
        int saved = errno;
        (void)sigaction(SIGSEGV, &old_segv, NULL);
        return -saved;
    }
    pto_cpu_probe_address = (uintptr_t)address;
    pto_cpu_probe_signal = pto_cpu_probe_code = 0;
    if (!sigsetjmp(pto_cpu_probe_jump, 1)) {
        pto_cpu_probe_armed = 1;
        *(volatile uint8_t *)address = original ^ 1U;
        pto_cpu_probe_armed = 0;
    } else {
        pto_cpu_probe_armed = 0;
        observed = 1;
    }
    int restore_error = 0;
    if (sigaction(SIGBUS, &old_bus, NULL)) restore_error = -errno;
    if (sigaction(SIGSEGV, &old_segv, NULL)) restore_error = -errno;
    if (restore_error) return restore_error;
    if (!observed || *(volatile uint8_t *)address != original) return -EPROTO;
    *fault = (struct lingqu_shmem_pto_cpu_fault) {
        .address = pto_cpu_probe_address,
        .signal = pto_cpu_probe_signal, .code = pto_cpu_probe_code,
    };
    return 0;
}

#endif
