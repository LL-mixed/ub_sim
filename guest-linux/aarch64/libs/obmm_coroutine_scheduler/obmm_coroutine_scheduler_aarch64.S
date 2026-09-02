/* SPDX-License-Identifier: MIT */

    .text
    .align 4

    .global obmm_coroutine_scheduler_upcall_entry
    .type obmm_coroutine_scheduler_upcall_entry, %function
obmm_coroutine_scheduler_upcall_entry:
    sub sp, sp, #832
    stp xzr, xzr, [sp, #0]
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    stp x8, x9, [sp, #80]
    stp x10, x11, [sp, #96]
    stp x12, x13, [sp, #112]
    stp x14, x15, [sp, #128]
    stp x16, x17, [sp, #144]
    stp x18, x19, [sp, #160]
    stp x20, x21, [sp, #176]
    stp x22, x23, [sp, #192]
    stp x24, x25, [sp, #208]
    stp x26, x27, [sp, #224]
    stp x28, x29, [sp, #240]
    str x30, [sp, #256]
    add x9, sp, #832
    str x9, [sp, #264]
    str xzr, [sp, #272]
    mrs x9, nzcv
    str x9, [sp, #280]
    stp q0, q1, [sp, #288]
    stp q2, q3, [sp, #320]
    stp q4, q5, [sp, #352]
    stp q6, q7, [sp, #384]
    stp q8, q9, [sp, #416]
    stp q10, q11, [sp, #448]
    stp q12, q13, [sp, #480]
    stp q14, q15, [sp, #512]
    stp q16, q17, [sp, #544]
    stp q18, q19, [sp, #576]
    stp q20, q21, [sp, #608]
    stp q22, q23, [sp, #640]
    stp q24, q25, [sp, #672]
    stp q26, q27, [sp, #704]
    stp q28, q29, [sp, #736]
    stp q30, q31, [sp, #768]
    mrs x9, fpcr
    str x9, [sp, #800]
    mrs x9, fpsr
    str x9, [sp, #808]
    mrs x9, tpidr_el0
    str x9, [sp, #816]
    str xzr, [sp, #824]
    mov x19, sp
    bl obmm_coroutine_scheduler_scheduler_stack_top
    cbz x0, .Lfatal
    mov sp, x0
    mov x0, x19
    bl obmm_coroutine_scheduler_upcall_dispatch
    b .Lfatal
    .size obmm_coroutine_scheduler_upcall_entry, .-obmm_coroutine_scheduler_upcall_entry

    .global obmm_coroutine_scheduler_context_bootstrap
    .type obmm_coroutine_scheduler_context_bootstrap, %function
obmm_coroutine_scheduler_context_bootstrap:
    mov x0, x19
    bl obmm_coroutine_scheduler_context_entry_c
    bl obmm_coroutine_scheduler_context_finish
    b .Lfatal
    .size obmm_coroutine_scheduler_context_bootstrap, .-obmm_coroutine_scheduler_context_bootstrap

    .global obmm_coroutine_scheduler_context_finish
    .type obmm_coroutine_scheduler_context_finish, %function
obmm_coroutine_scheduler_context_finish:
    bl obmm_coroutine_scheduler_scheduler_stack_top
    cbz x0, .Lfatal
    mov sp, x0
    bl obmm_coroutine_scheduler_schedule_after_exit
    b .Lfatal
    .size obmm_coroutine_scheduler_context_finish, .-obmm_coroutine_scheduler_context_finish

    .global obmm_coroutine_scheduler_context_resume
    .type obmm_coroutine_scheduler_context_resume, %function
obmm_coroutine_scheduler_context_resume:
    /* x0 = context image, x1 = one-shot replay token or zero. */
    mov x9, x0
    ldp q0, q1, [x9, #288]
    ldp q2, q3, [x9, #320]
    ldp q4, q5, [x9, #352]
    ldp q6, q7, [x9, #384]
    ldp q8, q9, [x9, #416]
    ldp q10, q11, [x9, #448]
    ldp q12, q13, [x9, #480]
    ldp q14, q15, [x9, #512]
    ldp q16, q17, [x9, #544]
    ldp q18, q19, [x9, #576]
    ldp q20, q21, [x9, #608]
    ldp q22, q23, [x9, #640]
    ldp q24, q25, [x9, #672]
    ldp q26, q27, [x9, #704]
    ldp q28, q29, [x9, #736]
    ldp q30, q31, [x9, #768]
    ldr x10, [x9, #800]
    msr fpcr, x10
    ldr x10, [x9, #808]
    msr fpsr, x10
    ldr x10, [x9, #816]
    msr tpidr_el0, x10
    mov x0, x9
    svc #0x5343
    b .Lfatal
    .size obmm_coroutine_scheduler_context_resume, .-obmm_coroutine_scheduler_context_resume

    .global obmm_coroutine_scheduler_wait_prepare
    .type obmm_coroutine_scheduler_wait_prepare, %function
obmm_coroutine_scheduler_wait_prepare:
    /* Clear the local event register before the final CQ-empty check. */
    sevl
    wfe
    ret
    .size obmm_coroutine_scheduler_wait_prepare, .-obmm_coroutine_scheduler_wait_prepare

    .global obmm_coroutine_scheduler_wait
    .type obmm_coroutine_scheduler_wait, %function
obmm_coroutine_scheduler_wait:
    wfe
    ret
    .size obmm_coroutine_scheduler_wait, .-obmm_coroutine_scheduler_wait

    .global obmm_coroutine_scheduler_scheduler_enter
    .type obmm_coroutine_scheduler_scheduler_enter, %function
obmm_coroutine_scheduler_scheduler_enter:
    svc #0x5345
    ret
    .size obmm_coroutine_scheduler_scheduler_enter, .-obmm_coroutine_scheduler_scheduler_enter

.Lfatal:
    brk #0
    b .Lfatal

    .section .note.GNU-stack,"",@progbits
