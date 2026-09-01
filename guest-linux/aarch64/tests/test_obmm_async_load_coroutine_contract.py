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


def test_async_load_uapi_v3_layout_compiles_for_aarch64():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    source = r"""
#include <stddef.h>
#include <ub/obmm_async_load.h>
_Static_assert(OBMM_ASYNC_LOAD_ABI_VERSION == 3, "ABI version");
_Static_assert(OBMM_ASYNC_LOAD_RESUME_HLT_IMM == 0x5343, "resume immediate");
_Static_assert(OBMM_ASYNC_LOAD_WAIT_HLT_IMM == 0x5344, "wait immediate");
_Static_assert(OBMM_ASYNC_LOAD_SCHEDULER_ENTER_HLT_IMM == 0x5345,
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
_Static_assert(sizeof(struct obmm_async_load_caps_v3) == 112, "caps size");
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
    assert "ub_async_load_replay_consume" in model
    assert "UB_ASYNC_LOAD_PLT_REPLAY_READY" in model
    assert "ub_async_load_cpu_take_upcall" in device
    assert "ub_async_load_cpu_resume" in device
    assert "ub_async_load_ring_publish_one" in device
    assert "ub_async_load_cpu_wait" in device
    assert "ub_async_load_cpu_scheduler_enter" in device
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
    assert "UB_ASYNC_LOAD_RESUME_IMM" in translate
    assert "UB_ASYNC_LOAD_WAIT_IMM" in translate
    assert "UB_ASYNC_LOAD_SCHEDULER_ENTER_IMM" in translate
    assert "gen_helper_async_load_resume" in translate
    assert "gen_helper_async_load_wait" in translate
    assert "gen_helper_async_load_scheduler_enter" in translate
    assert "HELPER(async_load_remote_load)" in helper
    assert "UB_ASYNC_LOAD_TRY_REPLAYED" in helper
    assert "async_load_replay_valid = true" in helper
    assert "env->pc = upcall_entry" in helper
    assert "HELPER(async_load_wait)" in helper
    assert "HELPER(async_load_scheduler_enter)" in helper
    assert "cpu_loop_exit_noexc(cs)" in helper
    wait_helper = helper.split("void HELPER(async_load_wait)", 1)[1].split(
        "void HELPER(async_load_scheduler_enter)", 1
    )[0]
    assert wait_helper.index("cs->halted = 1") < wait_helper.index(
        "qemu_mutex_unlock_iothread()"
    )
    assert "async_load_probe_access_range" in helper
    assert "probe_access(env, address, UB_ASYNC_LOAD_CONTEXT_BYTES" not in helper
    assert "async_load_context_load" in helper
    assert "async_load_context_install" in helper


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
    assert "target->context.x[event->rt] = event->value" in complete
    assert "target->context.pc = event->fault_pc + 4" in complete
    assert "runtime->replay_retire" in complete
    assert "runtime->caps.capabilities & OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE" in runtime
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
    assert ".inst 0xd44a6860" in assembly
    assert ".inst 0xd44a6880" in assembly
    assert ".inst 0xd44a68a0" in assembly
    assert "OBMM_ASYNC_LOAD_REG_UPCALL_ENTRY" in driver
    assert "OBMM_ASYNC_LOAD_REG_EVENT_RING_BASE" in driver
    assert "OBMM_ASYNC_LOAD_REG_EVENT_CONSUMER_BASE" in driver
    assert "struct obmm_async_load_start_v3" in driver
    assert "linqu_async_load_mmap" in driver
    assert "dma_mmap_coherent" in driver
    assert "linqu_async_load_create_context" not in driver
    assert "linqu_async_load_get_event" not in driver
    assert "linqu_async_load_scheduler_enter" not in driver
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


def test_async_load_hot_path_is_kernel_free_in_guest_abi_v3():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    runtime = (LIB_DIR / "obmm_coroutine_scheduler.c").read_text()

    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" not in uapi
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" not in uapi
    assert "linqu_async_load_get_event" not in driver
    assert "linqu_async_load_scheduler_enter" not in driver
    assert ".mmap = linqu_async_load_mmap" in driver
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" not in runtime
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" not in runtime
    assert "obmm_coroutine_scheduler_wait();" in runtime
    assert "obmm_coroutine_scheduler_scheduler_enter();" in runtime
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS" in uapi
    assert "case OBMM_ASYNC_LOAD_IOCTL_GET_REPLAY_STATS:" in driver
    assert "ctx->capabilities & OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE" in driver


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
    assert "--async-load-completion patch|replay" in app
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
    assert "wait_wakeup=hlt role=consumer" in app
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


def test_kernel_artifact_signature_tracks_async_load_v3_sources():
    builder = GUEST_ARTIFACT_BUILDER.read_text()
    signature = builder.split("current_kernel_artifact_signature()", 1)[1].split(
        "kernel_image_stamp_matches()", 1
    )[0]

    assert "include/linux/obmm.h" in signature
    assert "include/uapi/ub/obmm_async_load.h" in signature
    assert "arch/arm64/include/asm/esr.h" in signature
    assert "arch/arm64/mm/fault.c" in signature
    assert "include/linux/arm64_remote_load.h" in signature
    assert "linqu_driver_blob=" in signature
    assert 'git hash-object "$ROOT_DIR/driver/linqu_ub_drv.c"' in signature


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
    assert ".context_cookie = entry->load.context_cookie" in model
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
    assert "required_capabilities = OBMM_ASYNC_LOAD_CAP_KERNEL_FREE_EVENT_RING" in driver
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
    assert "scheduling=linux-task retirement=replay" in app
    assert "OBMM_ASYNC_LOAD_KERNEL_TASK_SUMMARY" in app
    assert "obmm_async_kernel_task_replay=1" in run_app
    assert 'args="$args --kernel-task-replay"' in run_app
    assert 'append_cmdline "obmm_async_kernel_task_replay=1"' in runner
    assert "OBMM_ASYNC_LOAD_KERNEL_TASK_EVIDENCE" in runner
    assert "remote-load block pid=" in runner


class ObmmAsyncLoadCoroutineContractTests(unittest.TestCase):
    def test_uapi_layout(self):
        test_async_load_uapi_v3_layout_compiles_for_aarch64()

    def test_cross_compile(self):
        test_async_load_library_and_shared_cli_cross_compile_without_warnings()

    def test_ordinary_load_data_plane(self):
        test_async_load_data_plane_is_an_ordinary_scalar_load()

    def test_qemu_mechanism_boundary(self):
        test_qemu_provides_mechanism_but_not_coroutine_policy()

    def test_guest_el0_scheduler_ownership(self):
        test_guest_el0_runtime_owns_save_state_and_selection()

    def test_kernel_free_hot_path(self):
        test_async_load_hot_path_is_kernel_free_in_guest_abi_v3()

    def test_async_load_producer_consumer_causal_evidence(self):
        test_async_load_producer_consumer_has_causal_upcall_evidence()

    def test_transport_neutrality(self):
        test_async_load_public_contract_is_transport_neutral()

    def test_scenario_contract(self):
        test_async_load_scenarios_do_not_model_qemu_scheduler_cycles()

    def test_kernel_artifact_signature(self):
        test_kernel_artifact_signature_tracks_async_load_v3_sources()

    def test_kernel_task_replay_contract(self):
        test_kernel_task_replay_uses_data_abort_cq_irq_and_linux_waitqueue()


if __name__ == "__main__":
    unittest.main()
