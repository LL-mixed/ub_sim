import importlib.util
import json
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
KERNEL_ROOT = ROOT.parent / "kernel_ub"
QEMU_ROOT = REPO_ROOT / "vendor" / "qemu_8.2.0_ub"
OBMM_ROOT = REPO_ROOT / "vendor" / "obmm"
LIB_DIR = ROOT / "libs" / "obmm_coroutine_scheduler"
ASYNC_LIB_DIR = ROOT / "libs" / "obmm_async"
APP_DIR = ROOT / "apps" / "obmm_async_coroutine"
GUEST_ARTIFACT_BUILDER = ROOT / "scripts" / "build_guest_artifacts.sh"


def test_async_load_control_v4_and_event_v3_layout_compile_for_aarch64():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    source = r"""
#include <stddef.h>
#include <ub/obmm_async_load.h>
_Static_assert(OBMM_ASYNC_LOAD_ABI_VERSION == 4, "control ABI version");
_Static_assert(OBMM_ASYNC_LOAD_EVENT_ABI_VERSION == 3, "event ABI version");
_Static_assert(OBMM_ASYNC_LOAD_RESUME_SVC_IMM == 0x5343, "resume immediate");
_Static_assert(OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM == 0x5345,
               "scheduler-enter immediate");
_Static_assert(OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE == (1ULL << 8),
               "replay capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING == (1ULL << 9),
               "event-ring capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_EL0_WAIT_WAKE == (1ULL << 10),
               "wait-wakeup capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_EL0_SCHEDULER_ENTER == (1ULL << 11),
               "scheduler-enter capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY == (1ULL << 12),
               "kernel-task replay capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN == (1ULL << 13),
               "NC replay-token capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_SVC_CONTEXT_RESUME == (1ULL << 14),
               "SVC resume capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_WFE_WAIT == (1ULL << 15),
               "WFE wait capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_CACHEABLE_FILL_REPLAY == (1ULL << 16),
               "Cacheable fill-replay capability");
_Static_assert(OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY == (1ULL << 17),
               "void-response retry capability");
_Static_assert(OBMM_ASYNC_LOAD_EVENT_CACHEABLE_FILL == (1U << 2),
               "Cacheable fill event flag");
_Static_assert(OBMM_ASYNC_LOAD_START_REPLAY_RETIRE == 1,
               "replay start flag");
_Static_assert(OBMM_ASYNC_LOAD_START_KERNEL_TASK == 2,
               "kernel-task start flag");
_Static_assert(sizeof(struct obmm_async_load_context_v2) == 832, "context size");
_Static_assert(offsetof(struct obmm_async_load_context_v2, x) == 16, "x offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, sp) == 264, "sp offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, pc) == 272, "pc offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, q) == 288, "q offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, fpcr) == 800,
               "fpcr offset");
_Static_assert(sizeof(struct obmm_async_load_caps_v4) == 112, "caps size");
_Static_assert(sizeof(struct obmm_async_load_map_register_v1) == 64, "map size");
_Static_assert(sizeof(struct obmm_async_load_start_v3) == 40, "start size");
_Static_assert(sizeof(struct obmm_async_load_event_producer_v3) == 64,
               "producer size");
_Static_assert(sizeof(struct obmm_async_load_event_consumer_v3) == 64,
               "consumer size");
_Static_assert(sizeof(struct obmm_async_load_event_v3) == 128, "event size");
_Static_assert(sizeof(struct obmm_async_load_stats_v3) == 152, "stats size");
_Static_assert(sizeof(struct obmm_async_load_observability_v3) == 144,
               "observability size");
_Static_assert(sizeof(struct obmm_async_load_replay_stats_v1) == 32,
               "replay stats size");
_Static_assert(sizeof(struct obmm_async_load_kernel_task_stats_v1) == 64,
               "kernel-task stats size");
_Static_assert(sizeof(struct obmm_async_load_path_stats_v1) == 48,
               "path stats size");
_Static_assert(offsetof(struct obmm_async_load_start_v3, upcall_entry) == 24,
               "upcall entry offset");
_Static_assert(offsetof(struct obmm_async_load_event_v3, interrupted_pc) == 32,
               "interrupted PC offset");
_Static_assert(offsetof(struct obmm_async_load_event_v3, map_id) == 64,
               "map id offset");
int main(void) { return 0; }
"""
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        source_path = directory / "layout.c"
        source_path.write_text(source)
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-Werror",
                "-idirafter",
                str(KERNEL_ROOT / "include" / "uapi"),
                "-c",
                str(source_path),
                "-o",
                str(directory / "layout.o"),
            ],
            check=True,
        )


def test_async_load_library_and_shared_cli_cross_compile_without_warnings():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "obmm_async_coroutine"
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fno-stack-protector",
                "-static",
                "-I",
                str(LIB_DIR),
                "-I",
                str(ASYNC_LIB_DIR),
                "-I",
                str(ROOT / "common"),
                "-I",
                str(OBMM_ROOT / "src" / "libobmm"),
                "-idirafter",
                str(KERNEL_ROOT / "include" / "uapi"),
                str(APP_DIR / "obmm_async_coroutine.c"),
                str(LIB_DIR / "obmm_coroutine_scheduler.c"),
                str(LIB_DIR / "obmm_coroutine_scheduler_aarch64.S"),
                str(ASYNC_LIB_DIR / "obmm_async.c"),
                str(ASYNC_LIB_DIR / "obmm_async_aarch64.S"),
                str(APP_DIR / "uffd_mode.c"),
                str(APP_DIR / "uffd_state.c"),
                str(ROOT / "common" / "obmm_uffd.c"),
                str(OBMM_ROOT / "src" / "libobmm" / "libobmm.c"),
                str(ROOT / "common" / "obmm_vendor_adaptor_sim.c"),
                "-pthread",
                "-o",
                str(output),
            ],
            check=True,
        )
        assert output.read_bytes()[:4] == b"\x7fELF"


def test_async_load_data_plane_is_an_ordinary_scalar_load():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    worker = app.split("static void async_load_worker_entry", 1)[1].split(
        "static int async_compare_u64", 1
    )[0]
    scalar_load = app.split(
        "static __attribute__((noinline)) uint64_t async_load_scalar_load", 1
    )[1].split("static void async_load_worker_entry", 1)[0]

    assert "async_load_scalar_load(address" in worker
    assert worker.count("async_worker_now_ns(worker)") == 4
    assert "obmm_load_submit" not in worker
    assert "obmm_await" not in worker
    for scalar_type in ("uint8_t", "uint16_t", "uint32_t", "uint64_t"):
        assert f"const volatile {scalar_type} *" in scalar_load


def test_qemu_provides_mechanism_but_not_coroutine_policy():
    model_header = (QEMU_ROOT / "include" / "hw" / "ub" / "ub_async_load.h").read_text()
    model = (QEMU_ROOT / "hw" / "ub" / "ub_async_load.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_async_load_device.c").read_text()
    translate = (QEMU_ROOT / "target" / "arm" / "tcg" / "translate-a64.c").read_text()
    helper = (QEMU_ROOT / "target" / "arm" / "tcg" / "helper-a64.c").read_text()

    assert "UbAsyncLoadEvent" in model_header
    assert "UbAsyncLoadArchState" not in model_header
    assert "obmm_coroutine_scheduler_schedule_next" not in model_header
    assert "obmm_coroutine_scheduler_context_create" not in model_header
    assert "ub_async_load_event_pop" in model
    assert "ub_async_load_replay_consume_token" in model
    assert "ub_async_load_replay_arm" in model
    assert "UB_ASYNC_LOAD_PLT_REPLAY_READY" in model
    assert "UbAsyncLoadNcPltEntry" in model
    assert "ub_async_load_cpu_take_upcall" in device
    assert "ub_async_load_cpu_resume" in device
    assert "ub_async_load_ring_publish_one" in device
    assert "ub_async_load_cpu_scheduler_enter" in device
    assert "ASYNC_LOAD_REG_REPLAY_TOKEN" in device
    assert "state->armed_replay_token" in device
    replay_expected = device.split(
        "bool ub_async_load_cpu_replay_expected", 1
    )[1].split("bool ub_async_load_cpu_take_upcall", 1)[0]
    assert "!state->upcall_active" in replay_expected
    status_read = device.split("case ASYNC_LOAD_REG_STATUS:", 1)[1].split(
        "case ASYNC_LOAD_REG_LAST_ERROR:", 1
    )[0]
    assert "obmm_remote_run_deadlines(" in status_read
    assert "obmm_remote_deliver_ready(state->backend);" in status_read
    assert "ub_async_load_arm_deadline(state);" in status_read
    assert "status_probe_reads" not in device
    assert "status-no-progress" not in device
    assert "active_context_id = context_id" in device
    assert "active_context_id && !state->upcall_active" not in device
    assert "ready_queue" not in device
    assert "UB_ASYNC_LOAD_RESUME_IMM" not in translate
    assert "UB_ASYNC_LOAD_WAIT_IMM" not in translate
    assert "UB_ASYNC_LOAD_SCHEDULER_ENTER_IMM" not in translate
    assert "gen_helper_async_load_resume" not in translate
    assert "gen_helper_async_load_wait" not in translate
    assert "gen_helper_async_load_scheduler_enter" not in translate
    assert "HELPER(async_load_remote_load)" in helper
    assert "UB_ASYNC_LOAD_TRY_REPLAYED" in helper
    assert "async_load_replay_valid = true" in helper
    remote_load_helper = helper.split(
        "uint64_t HELPER(async_load_remote_load)", 1
    )[1].split("#else", 1)[0]
    assert remote_load_helper.index(
        "ub_async_load_cpu_select_kernel_context"
    ) < remote_load_helper.index("ub_async_load_cpu_address_is_remote")
    assert "env->pc = upcall_entry" in helper
    assert "HELPER(async_load_wait)" not in helper
    assert "HELPER(async_load_scheduler_enter)" not in helper
    assert "cpu_loop_exit_noexc(cs)" in helper
    assert "async_load_probe_access_range" in helper
    assert "probe_access(env, address, UB_ASYNC_LOAD_CONTEXT_BYTES" not in helper
    assert "async_load_context_load" not in helper
    assert "async_load_context_install" not in helper


def test_guest_el0_runtime_owns_save_state_and_selection():
    runtime = (LIB_DIR / "obmm_coroutine_scheduler.c").read_text()
    dispatch = runtime.split("void obmm_coroutine_scheduler_upcall_dispatch", 1)[1].split(
        "void obmm_coroutine_scheduler_context_entry_c", 1
    )[0]
    assembly = (LIB_DIR / "obmm_coroutine_scheduler_aarch64.S").read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_async_load_device.c").read_text()

    assert "OBMM_COROUTINE_SCHEDULER_CONTEXT_READY" in runtime
    assert "OBMM_COROUTINE_SCHEDULER_CONTEXT_WAIT_REMOTE" in runtime
    assert "obmm_coroutine_scheduler_choose_ready" in runtime
    assert "obmm_coroutine_scheduler_process_event" in runtime
    assert "OBMM_COROUTINE_SCHEDULER_PROTOCOL_ERROR schema=1" in runtime
    assert "OBMM_COROUTINE_SCHEDULER_CONTEXT_STATE schema=1" in runtime
    assert "if (runtime->first_error)" in runtime
    complete = runtime.split("case OBMM_ASYNC_LOAD_EVENT_COMPLETE:", 1)[1].split(
        "case OBMM_ASYNC_LOAD_EVENT_FAULT:", 1
    )[0]
    assert "OBMM_COROUTINE_SCHEDULER_CONTEXT_READY_REPLAY" in complete
    assert "target->replay_token = event->plt_token" in complete
    assert "target->context.x[event->rt] = event->value" not in complete
    assert "target->context.pc = event->fault_pc + 4" not in complete
    assert "complete-replay-contract" in complete
    assert "OBMM_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN" in runtime
    assert "runtime->current->state != OBMM_COROUTINE_SCHEDULER_CONTEXT_DONE" in dispatch
    assert "interrupted_was_running" in dispatch
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" not in runtime
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" not in runtime
    assert "obmm_coroutine_scheduler_event_ring_next" in runtime
    assert "obmm_coroutine_scheduler_event_ring_drain" in runtime
    assert "__ATOMIC_ACQUIRE" in runtime
    assert "__ATOMIC_RELEASE" in runtime
    assert "runtime->metrics.kernel_hotpath_ioctls++" in runtime
    assert "runtime->hot_path_active = true" in runtime
    assert "runtime->hot_path_active = false" in runtime
    assert "stp x0, x1, [sp, #16]" in assembly
    assert "stp q30, q31, [sp, #768]" in assembly
    assert "svc #0x5343" in assembly
    assert "svc #0x5345" in assembly
    assert "obmm_coroutine_scheduler_wait_prepare:" in assembly
    assert "sevl" in assembly
    assert "wfe" in assembly
    assert "ldr x10, [x9, #816]" in assembly
    assert "msr tpidr_el0, x10" in assembly
    assert ".inst 0xd44a6860" not in assembly
    assert ".inst 0xd44a6880" not in assembly
    assert ".inst 0xd44a68a0" not in assembly
    assert "OBMM_ASYNC_LOAD_REG_UPCALL_ENTRY" in driver
    assert "OBMM_ASYNC_LOAD_REG_EVENT_RING_BASE" in driver
    assert "OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BASE" in driver
    assert "OBMM_ASYNC_LOAD_REG_REPLAY_TOKEN" in driver
    assert "linqu_remote_load_svc" in driver
    assert "struct obmm_async_load_start_v3" in driver
    assert "linqu_async_load_mmap" in driver
    assert "dma_mmap_coherent" in driver
    assert "linqu_async_load_create_context" not in driver
    assert "linqu_async_load_get_event" not in driver
    assert "linqu_async_load_scheduler_enter_command" in driver
    ring_mmap = driver.split("static int linqu_async_load_mmap", 1)[1].split(
        "static long linqu_async_load_query_caps", 1
    )[0]
    assert "vma->vm_flags & VM_WRITE" in ring_mmap
    publish = device.split("static bool ub_async_load_ring_publish_one", 1)[1].split(
        "static void ub_async_load_reset", 1
    )[0]
    assert publish.index("dma_memory_write(") < publish.index(
        "offsetof(UbAsyncLoadEventProducerV3, producer_sequence)"
    )
    schedule_after_exit = runtime.split(
        "void obmm_coroutine_scheduler_schedule_after_exit", 1
    )[1].split("static int obmm_coroutine_scheduler_collect_metrics", 1)[0]
    assert schedule_after_exit.index("obmm_coroutine_scheduler_scheduler_enter()") < (
        schedule_after_exit.index("obmm_coroutine_scheduler_schedule(runtime")
    )


def test_event_handling_stays_in_el0_with_narrow_svc_resume_assists():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    runtime = (LIB_DIR / "obmm_coroutine_scheduler.c").read_text()

    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" not in uapi
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" not in uapi
    assert "linqu_async_load_get_event" not in driver
    assert "linqu_async_load_scheduler_enter_command" in driver
    assert ".mmap = linqu_async_load_mmap" in driver
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" not in runtime
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" not in runtime
    assert "obmm_coroutine_scheduler_wait();" in runtime
    assert "obmm_coroutine_scheduler_wait_prepare();" in runtime
    wait_path = runtime.split(
        "obmm_coroutine_scheduler_wait_prepare();", 1
    )[1].split("continue;", 1)[0]
    assert wait_path.index(
        "obmm_coroutine_scheduler_event_ring_drain(runtime)"
    ) < wait_path.index("obmm_coroutine_scheduler_ready_count(runtime)")
    assert "obmm_coroutine_scheduler_scheduler_enter();" in runtime
    assert "svc #0x5343" in (
        LIB_DIR / "obmm_coroutine_scheduler_aarch64.S"
    ).read_text()
    assert "wfe" in (
        LIB_DIR / "obmm_coroutine_scheduler_aarch64.S"
    ).read_text()
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS" in uapi
    assert "case OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS:" in driver
    assert "OBMM_ASYNC_LOAD_CAP_SVC_CONTEXT_RESUME" in driver


def test_async_load_producer_consumer_has_causal_upcall_evidence():
    public = (LIB_DIR / "obmm_coroutine_scheduler.h").read_text()
    runtime = (LIB_DIR / "obmm_coroutine_scheduler.c").read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()

    assert "struct obmm_coroutine_scheduler_trace_event" in public
    assert "obmm_coroutine_scheduler_trace_fn trace" in public
    for event in (
        "OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_PENDING",
        "OBMM_COROUTINE_SCHEDULER_TRACE_UPCALL_COMPLETE",
        "OBMM_COROUTINE_SCHEDULER_TRACE_CONTEXT_RESUME",
    ):
        assert event in runtime
    assert "--async-load-producer-consumer" in app
    assert "--async-load-completion replay" in app
    assert "async_load_completion=%s replay_consumed=" in app
    assert "async_run_async_load_producer" in app
    assert "async_run_async_load_consumer" in app
    assert "OBMM_ASYNC_LOAD_WRITE schema=1" in app
    assert "OBMM_ASYNC_LOAD_LDR schema=1 event=issue" in app
    assert "OBMM_ASYNC_LOAD_UPCALL schema=1 event=pending" in app
    assert "OBMM_ASYNC_LOAD_UPCALL schema=1 event=complete" in app
    assert "OBMM_ASYNC_LOAD_COROUTINE_SUMMARY schema=1" in app
    assert "OBMM_ASYNC_LOAD_SUMMARY schema=1" in app
    assert "abi=%u event_delivery=ring" in app
    assert "wait_wakeup=wfe-irq role=consumer" in app
    assert runner.count(
        '$(summary_field "$async_load_summary" abi)" != "4"'
    ) == 2
    assert (
        '$(summary_field "$async_load_summary" wait_wakeup)" '
        '!= "wfe-irq"'
    ) in runner
    assert '$(summary_field "$async_load_summary" abi)" != "3"' not in runner
    assert '$(summary_field "$async_load_summary" wait_wakeup)" != "hlt"' not in runner
    assert (
        '$(summary_field "$complete_line" value)" '
        '!= "0000000000000000"'
    ) in runner
    assert (
        '$(summary_field "$complete_line" value)" != "$expected_value"'
        not in runner
    )
    assert "kernel_hotpath_ioctls" in app
    assert "ASYNC_LOAD coroutine $coroutine_id causal event order is invalid" in runner
    assert "OBMM_ASYNC_LOAD_CAUSAL_SUMMARY" in runner
    assert "blocked load switching to another coroutine" in runner
    assert "source_export_mem_id" in runner


def test_async_load_public_contract_is_transport_neutral():
    public_text = (LIB_DIR / "obmm_coroutine_scheduler.h").read_text().lower()
    uapi_text = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text().lower()

    for forbidden in ("sim_dec", "urma", "rdma", "roce", "tcp", "cuda"):
        assert forbidden not in public_text
        assert forbidden not in uapi_text


def test_async_load_scenarios_do_not_model_qemu_scheduler_cycles():
    for scenario in (REPO_ROOT / "scenarios").glob("mvp_*host_*.yaml"):
        text = scenario.read_text()
        assert "async_load_model:" in text
        assert "context_entries: 64" in text
        assert "pending_load_entries: 64" in text
        assert "event_queue_depth: 128" in text
        for stale in (
            "save_cycles:",
            "schedule_cycles:",
            "restore_cycles:",
            "commit_cycles:",
        ):
            assert stale not in text


def test_kernel_artifact_signature_tracks_async_load_v4_sources():
    builder = GUEST_ARTIFACT_BUILDER.read_text()
    signature = builder.split("current_kernel_artifact_signature()", 1)[1].split(
        "kernel_image_stamp_matches()", 1
    )[0]

    assert "include/linux/obmm.h" in signature
    assert "include/uapi/ub/obmm_async_load.h" in signature
    assert "arch/arm64/include/asm/esr.h" in signature
    assert "arch/arm64/kernel/syscall.c" in signature
    assert "arch/arm64/mm/fault.c" in signature
    assert "include/linux/arm64_remote_load.h" in signature
    assert "linqu_driver_blob=" in signature
    assert 'git hash-object "$ROOT_DIR/driver/linqu_ub_drv.c"' in signature


def test_private_svc_hook_bypasses_standard_svc_zero():
    syscall = (
        KERNEL_ROOT / "arch" / "arm64" / "kernel" / "syscall.c"
    ).read_text()
    fault = (KERNEL_ROOT / "arch" / "arm64" / "mm" / "fault.c").read_text()
    fault_api = (
        KERNEL_ROOT / "include" / "linux" / "arm64_remote_load.h"
    ).read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()

    svc = syscall.split("void do_el0_svc", 1)[1].split(
        "#ifdef CONFIG_AARCH32_EL0", 1
    )[0]
    assert "if (imm)" in svc
    assert svc.index("if (imm)") < svc.index("arm64_handle_remote_load_svc")
    assert "remote_load_ret != -ENOENT" in svc
    assert "handle_svc" in fault_api
    assert "try_module_get(ops->owner)" in fault
    assert "imm != OBMM_ASYNC_LOAD_RESUME_SVC_IMM" in driver
    assert "imm != OBMM_ASYNC_LOAD_SCHEDULER_ENTER_SVC_IMM" in driver


def test_kernel_task_replay_uses_data_abort_cq_irq_and_linux_waitqueue():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    esr = (KERNEL_ROOT / "arch" / "arm64" / "include" / "asm" / "esr.h").read_text()
    fault = (KERNEL_ROOT / "arch" / "arm64" / "mm" / "fault.c").read_text()
    fault_api = (KERNEL_ROOT / "include" / "linux" / "arm64_remote_load.h").read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_async_load_device.c").read_text()
    model = (QEMU_ROOT / "hw" / "ub" / "ub_async_load.c").read_text()
    ubc = (QEMU_ROOT / "hw" / "ub" / "ub_ubc.c").read_text()
    virt = (QEMU_ROOT / "hw" / "arm" / "virt.c").read_text()
    helper = (QEMU_ROOT / "target" / "arm" / "tcg" / "helper-a64.c").read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()

    assert "OBMM_ASYNC_LOAD_CAP_KERNEL_TASK_REPLAY" in uapi
    assert "OBMM_ASYNC_LOAD_START_KERNEL_TASK" in uapi
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_KERNEL_TASK_STATS" in uapi
    assert "ESR_ELx_FSC_REMOTE_LOAD\t(0x3a)" in esr
    assert "arm64_register_remote_load_fault_handler" in fault_api
    assert "do_remote_load_fault" in fault
    assert '"UB remote load pending"' in fault

    assert "env->cp15.tpidr_el[0]" in helper
    assert "ub_async_load_cpu_take_kernel_fault" in helper
    assert "UB_ASYNC_LOAD_REMOTE_FSC" in helper
    assert "raise_exception_ra(env, EXCP_DATA_ABORT" in helper
    kernel_fault = helper.split(
        "if (kernel_task) {", 1
    )[1].split("env->pc = upcall_entry", 1)[0]
    assert "env->exception.vaddress = va" in kernel_fault
    assert "env->pc" not in kernel_fault

    assert "ASYNC_LOAD_REG_IRQ_STATUS" in device
    assert ".context_cookie = load->context_cookie" in model
    assert ".reserved[0] = cpu_to_le64(" in device
    assert "ubc_async_load_irq_set(state->ubc_dev, true)" in device
    assert "ubc_async_load_irq_set(state->ubc_dev, state->irq_status != 0)" in device
    assert "state->event_consumer_sequence !=" in device
    assert "qemu_set_irq(bcs->async_load_irq, level)" in ubc
    assert 'qemu_fdt_setprop_cells(ms->fdt, linqu_nodename, "interrupts"' in virt
    assert "vms->irqmap[VIRT_PLATFORM_BUS] + 1" in virt
    assert "linqu_remote_load_fault" in driver
    assert "linqu_async_load_drain_kernel_events_locked" in driver
    assert "wait_event_killable_timeout" in driver
    assert "wake_up(&wait->waitq)" in driver
    assert "read_sysreg(tpidr_el0)" in driver
    assert "candidate->context_cookie == context_cookie" in driver
    assert "wait->context_cookie != context_cookie" in driver
    assert "wait->fault_pc != fault_pc" in driver
    assert "wait->effective_va != effective_va" in driver
    assert "OBMM_ASYNC_LOAD_CAP_NC_REPLAY_TOKEN" in driver
    assert "OBMM_ASYNC_LOAD_REG_IRQ_ACK" in driver
    remote_fault = driver.split(
        "static int linqu_remote_load_fault", 1
    )[1].split("static const struct arm64_remote_load_fault_ops", 1)[0]
    assert "regs->pc" in remote_fault
    assert "regs->pc =" not in remote_fault
    assert "regs->pc +=" not in remote_fault

    assert "--kernel-task-replay" in app
    assert "--threads N" in app
    assert "pthread_create" in app
    assert "async_load_scalar_load(address" in app
    assert 'void_response_retry ? "runnable-yield" : "linux-task"' in app
    assert '"retirement=replay late_completion=%s "' in app
    assert "OBMM_ASYNC_LOAD_KERNEL_TASK_SUMMARY" in app
    assert "obmm_async_kernel_task_replay=1" in run_app
    assert 'args="$args --kernel-task-replay"' in run_app
    assert 'append_cmdline "obmm_async_kernel_task_replay=1"' in runner
    assert "OBMM_ASYNC_LOAD_KERNEL_TASK_EVIDENCE" in runner
    assert "remote-load block path=.* pid=" in runner


def test_void_response_policy_controls_wire_fault_and_tokenless_retry():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    policy_header = (
        QEMU_ROOT / "include" / "hw" / "ub" / "ub_void_response_policy.h"
    ).read_text()
    policy = (QEMU_ROOT / "hw" / "ub" / "ub_void_response_policy.c").read_text()
    ubc_header = (QEMU_ROOT / "include" / "hw" / "ub" / "ub_ubc.h").read_text()
    ubc = (QEMU_ROOT / "hw" / "ub" / "ub_ubc.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_async_load_device.c").read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    launcher = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()

    assert "OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY" in uapi
    assert "UbVoidResponsePolicyConfig" in policy_header
    assert "threshold_ns" in policy_header
    assert "latency_ns" in policy_header
    assert "jitter_ns" in policy_header
    assert "fault_voids" in policy_header
    assert "ub_void_response_policy_decide" in policy
    assert "UB_VOID_RESPONSE_FAULT_INJECTION" in policy
    assert "UBC_SIM_DEC_READ_FLAG_VOID_ELIGIBLE" in ubc_header
    assert "UBC_SIM_DEC_READ_STATUS_VOID" in ubc_header
    assert "UB_VOID_RESPONSE_DECISION" in ubc
    assert "UB_VOID_RESPONSE_TX" in ubc
    assert "UB_VOID_RESPONSE_RX" in ubc
    assert "UB_VOID_RESPONSE_CPU_RAISE" in ubc
    assert "UB_VOID_RESPONSE_LOCAL_TRIGGER" in ubc
    assert "UB_VOID_RESPONSE_TRIGGER_RACE_LOST" in ubc
    assert "UB_VOID_RESPONSE_LATE_DROP" in ubc
    assert "ubc_obmm_async_child_try_void" in ubc
    assert "UBC_VOID_TRIGGER_SOURCE_POLICY" in ubc
    assert "UBC_VOID_TRIGGER_REMOTE_WIRE" in ubc
    assert "child->voided = true" in ubc
    late_drop = ubc.split("static void ubc_voided_transaction_bh", 1)[1].split(
        "static bool ubc_handle_sim_dec_async_read_resp", 1
    )[0]
    assert "OBMM_REMOTE_STATUS_VOIDED" in late_drop
    assert "child->complete" in late_drop
    assert "ubc_async_load_irq_set" not in late_drop
    assert "ub_async_load_cpu_take_kernel_fault" in device
    tokenless_complete = device.split(
        "if (result->status == OBMM_REMOTE_STATUS_VOIDED)", 1
    )[1].split("ub_async_load_complete_future", 1)[0]
    assert "ub_async_load_future_release" in tokenless_complete
    assert "ubc_async_load_irq_set" not in tokenless_complete
    remote_fault = driver.split(
        "static int linqu_remote_load_fault", 1
    )[1].split("static const struct arm64_remote_load_fault_ops", 1)[0]
    assert "OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY" in remote_fault
    assert "set_current_state(TASK_RUNNING)" in remote_fault
    assert "schedule();" in remote_fault
    tokenless_fault = remote_fault.split(
        "OBMM_ASYNC_LOAD_CAP_VOID_RESPONSE_RETRY", 1
    )[1].split("mutex_lock(&ctx->lock);", 2)[1]
    assert "wait_event" not in tokenless_fault
    assert "wait_key" not in tokenless_fault
    assert "--void-response-policy" in launcher
    assert "ubc.void-response-policy=" in launcher
    assert "--source-void-response-policy" in launcher
    assert "ubc.source-void-response-policy=" in launcher


def test_normal_cacheable_void_response_uses_fill_replay_without_nc_plt():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_async_load_device.c").read_text()
    model = (QEMU_ROOT / "hw" / "ub" / "ub_async_load.c").read_text()
    ubc = (QEMU_ROOT / "hw" / "ub" / "ub_ubc.c").read_text()
    helper = (QEMU_ROOT / "target" / "arm" / "tcg" / "helper-a64.c").read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()

    assert "OBMM_ASYNC_LOAD_CAP_CACHEABLE_FILL_REPLAY" in uapi
    assert "OBMM_ASYNC_LOAD_EVENT_CACHEABLE_FILL" in uapi
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_PATH_STATS" in uapi
    assert "load.normal_cacheable = async_load_probe_access_range" in helper
    assert "full->extra.arm.pte_attrs" in helper
    assert "ub_async_load_cacheable_pending(" in model
    assert "ub_async_load_cacheable_complete(" in model
    assert "stats.nc_plt_allocations++" in model
    cache_pending = model.split(
        "UbAsyncLoadPendingResult ub_async_load_cacheable_pending", 1
    )[1].split("UbAsyncLoadCompletionResult ub_async_load_cacheable_complete", 1)[0]
    assert "nc_plt" not in cache_pending
    assert "ubc_obmm_cacheable_fill_lookup(" in ubc
    assert "ubc_obmm_cacheable_fill_complete(" in ubc
    assert "#define SIM_DEC_CACHE_LINE_SIZE     64" in ubc
    assert "uint64_t valid_line_mask;" in ubc
    assert "sim_dec_page_cache_range_valid(cache_entry" in ubc
    assert "ASYNC_LOAD_CACHEABLE_PENDING" in device
    assert "ASYNC_LOAD_CACHEABLE_FILL" in device
    assert "ASYNC_LOAD_CACHEABLE_REPLAY_HIT" in device
    assert "UB_ASYNC_LOAD_FUTURE_CACHEABLE" in device
    assert "UB_ASYNC_LOAD_CAP_CACHEABLE_FILL_REPLAY" in device
    assert "wait->cacheable ? 0 : linqu_async_load_resume_command" in driver
    assert "esr=0x%lx fsc=0x%lx" in driver
    assert 'wait->cacheable ? "eret-replay" : "nc-replay-command"' in driver
    assert "OBMM_ASYNC_LOAD_REG_PATH_STATS_BASE" in driver
    assert "--async-load-memory normal-nc|normal-cacheable" in app
    assert "OBMM_IMPORT_CACHE_CC" in app
    assert "path_stats.nc_plt_allocations" in app
    assert "cacheable_fill_completed" in app
    assert "obmm_async_load_memory=" in run_app
    assert "--async-load-memory must be normal-nc or normal-cacheable" in runner
    assert "SIM_DEC_PAGE_CACHE_PER_MAP=64" in runner
    assert "async_load_coroutines * 64" in runner
    assert "OBMM_ASYNC_LOAD_CACHEABLE_EVIDENCE" in runner
    assert "remote-load block path=normal-cacheable .* fsc=0x3a" in runner
    assert "resume=eret-replay result=0" in runner
    assert "cacheable_pending_first >= cacheable_submit_first" in runner
    assert "cacheable_submit_first >= cacheable_fill_first" in runner
    assert "pending_before_remote_submit=1" in runner


def test_async_load_timed_run_can_disable_per_event_logging():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()

    assert "--async-load-event-log on|off" in app
    assert "if (!app->config.async_load_event_log)" in app
    assert ".trace = app->config.async_load_event_log ?" in app
    assert "async_load_coroutine_trace : NULL" in app
    assert "bool trace_pass = !app->config.async_load_event_log" in app
    assert "causal_timing_pass = !app->config.async_load_event_log" in app
    assert app.count("if (app->config.async_load_event_log)") >= 2
    assert "app->latencies_ns[iteration] = latency_ns" in app
    assert "worker->async_load_operations_completed++" in app
    assert "app->latency_count += worker->async_load_operations_completed" in app
    assert "/sys/module/linqu_ub_drv/parameters/remote_load_event_log" in app
    assert "module_param(remote_load_event_log, bool, 0644)" in driver
    assert driver.count("if (remote_load_event_log)") >= 4
    assert "obmm_async_load_event_log=" in run_app
    assert "--async-load-event-log must be on or off" in runner
    assert "ASYNC_LOAD direct-EL0 event log remained active" in runner
    assert "ASYNC_LOAD kernel-task event log remained active" in runner
    assert "ASYNC_LOAD producer/consumer causal timing evidence is incomplete" in runner


def test_async_load_trace_off_compare_cli_expands_paired_replay_cases():
    script = ROOT / "scripts" / "run_ub_async_load_scheduler_compare.py"
    base_model = {
        "schema": 1,
        "manifest_hash": "fnv1a64:0000000000000000",
        "scenario_name": "mvp_2host_async_load_remote_10ms",
        "scenario_seed": 42,
        "remote_memory_model": {
            "enabled": True,
            "time_source": "qemu_virtual",
            "fixed_latency_ns": 10_000_000,
            "jitter": {"mode": "none", "max_abs_ns": 0},
            "tail": {"probability_ppm": 0, "extra_latency_ns": 0},
            "queue_depth": 64,
            "reorder_window": 1,
            "drop_ppm": 0,
            "error_ppm": 0,
            "duplicate_ppm": 0,
            "duplicate_delay_ns": 1_000,
            "seed": 1,
        },
    }
    with tempfile.TemporaryDirectory() as directory:
        directory = Path(directory)
        model_path = directory / "base-model.json"
        output_dir = directory / "comparison"
        model_path.write_text(json.dumps(base_model))
        result = subprocess.run(
            [
                "python3",
                str(script),
                "--scenario-config",
                str(REPO_ROOT / "scenarios/mvp_2host_async_load_remote_10ms.yaml"),
                "--base-model-manifest",
                str(model_path),
                "--output-dir",
                str(output_dir),
                "--latencies-us",
                "1,10",
                "--seeds",
                "1",
                "--contexts",
                "4",
                "--operations",
                "8",
                "--dry-run",
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        dry_runs = [
            line for line in result.stdout.splitlines() if line.startswith("DRY_RUN ")
        ]
        manifest = json.loads((output_dir / "run-manifest.json").read_text())
        validation = json.loads((output_dir / "validation.json").read_text())

    assert len(dry_runs) == 4
    assert all("--async-load-event-log off" in line for line in dry_runs)
    assert all("--async-load-completion replay" in line for line in dry_runs)
    assert sum("--kernel-task-replay" in line for line in dry_runs) == 2
    assert manifest["event_log"] == "off"
    assert manifest["event_trace_callback"] == "off"
    assert manifest["completion"] == "replay"
    assert validation == {
        "schema": 1,
        "status": "dry-run",
        "completed": 0,
        "planned": 4,
    }


def test_async_load_trace_off_compare_validates_pairs_and_campaign_artifacts():
    script = ROOT / "scripts" / "run_ub_async_load_scheduler_compare.py"
    spec = importlib.util.spec_from_file_location("scheduler_compare", script)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    common = {
        "latency_us": 10,
        "seed": 1,
        "contexts": 4,
        "operations": 256,
        "checksum": "0000000000000600",
        "model_contract_hash": "fnv1a64:1234",
        "scenario_sha256": "scenario",
        "qemu_sha256": "qemu",
        "kernel_sha256": "kernel",
        "initramfs_sha256": "initramfs",
    }
    rows = [
        {
            **common,
            "mode": "el0-coroutine",
            "makespan_ns": 100,
            "guest_ns_p50": 10,
            "guest_ns_p95": 15,
            "guest_ns_p99": 20,
            "operations_per_second": 2.0,
        },
        {
            **common,
            "mode": "kernel-task",
            "makespan_ns": 125,
            "guest_ns_p50": 12,
            "guest_ns_p95": 24,
            "guest_ns_p99": 30,
            "operations_per_second": 1.6,
        },
    ]

    pairs = module.validate_pairs(rows)
    fingerprints = module.validate_campaign_artifacts(rows)
    aggregate = module.aggregate_pairs(pairs)

    assert pairs[0]["kernel_over_el0_makespan_ratio"] == 1.25
    assert aggregate[0]["kernel_over_el0_p50_ratio_median"] == 1.2
    assert aggregate[0]["kernel_over_el0_p95_ratio_median"] == 1.6
    assert aggregate[0]["kernel_over_el0_p99_ratio_median"] == 1.5
    assert fingerprints["qemu_sha256"] == "qemu"
    with tempfile.TemporaryDirectory() as directory:
        failed_log = Path(directory) / "case.log"
        failed_log.write_text("failed\n")
        preserved = module.preserve_failed_log(failed_log)
        assert preserved.name == "case.attempt-1.log"
        assert preserved.read_text() == "failed\n"
        assert not failed_log.exists()
    rows[1]["qemu_sha256"] = "changed"
    try:
        module.validate_campaign_artifacts(rows)
    except ValueError:
        pass
    else:
        raise AssertionError("campaign artifact drift was accepted")


class ObmmAsyncLoadCoroutineContractTests(unittest.TestCase):
    def test_uapi_layout(self):
        test_async_load_control_v4_and_event_v3_layout_compile_for_aarch64()

    def test_cross_compile(self):
        test_async_load_library_and_shared_cli_cross_compile_without_warnings()

    def test_ordinary_load_data_plane(self):
        test_async_load_data_plane_is_an_ordinary_scalar_load()

    def test_qemu_mechanism_boundary(self):
        test_qemu_provides_mechanism_but_not_coroutine_policy()

    def test_guest_el0_scheduler_ownership(self):
        test_guest_el0_runtime_owns_save_state_and_selection()

    def test_kernel_free_event_handling(self):
        test_event_handling_stays_in_el0_with_narrow_svc_resume_assists()

    def test_async_load_producer_consumer_causal_evidence(self):
        test_async_load_producer_consumer_has_causal_upcall_evidence()

    def test_transport_neutrality(self):
        test_async_load_public_contract_is_transport_neutral()

    def test_scenario_contract(self):
        test_async_load_scenarios_do_not_model_qemu_scheduler_cycles()

    def test_kernel_artifact_signature(self):
        test_kernel_artifact_signature_tracks_async_load_v4_sources()

    def test_private_svc_hook(self):
        test_private_svc_hook_bypasses_standard_svc_zero()

    def test_kernel_task_replay_contract(self):
        test_kernel_task_replay_uses_data_abort_cq_irq_and_linux_waitqueue()

    def test_void_response_policy_contract(self):
        test_void_response_policy_controls_wire_fault_and_tokenless_retry()

    def test_normal_cacheable_fill_replay_contract(self):
        test_normal_cacheable_void_response_uses_fill_replay_without_nc_plt()

    def test_trace_off_contract(self):
        test_async_load_timed_run_can_disable_per_event_logging()

    def test_trace_off_compare_cli(self):
        test_async_load_trace_off_compare_cli_expands_paired_replay_cases()

    def test_trace_off_compare_aggregation(self):
        test_async_load_trace_off_compare_validates_pairs_and_campaign_artifacts()


if __name__ == "__main__":
    unittest.main()
