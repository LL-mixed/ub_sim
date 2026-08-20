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
    /* Custom simulated resume primitive: HLT #0x5343, x0 = context. */
    .inst 0xd44a6860
    b .Lfatal
    .size obmm_coroutine_scheduler_context_resume, .-obmm_coroutine_scheduler_context_resume

.Lfatal:
    brk #0
    b .Lfatal

    .section .note.GNU-stack,"",@progbits
