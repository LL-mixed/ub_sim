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


def test_async_load_uapi_v2_layout_compiles_for_aarch64():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    source = r"""
#include <stddef.h>
#include <ub/obmm_async_load.h>
_Static_assert(OBMM_ASYNC_LOAD_ABI_VERSION == 2, "ABI version");
_Static_assert(OBMM_ASYNC_LOAD_RESUME_HLT_IMM == 0x5343, "resume immediate");
_Static_assert(OBMM_ASYNC_LOAD_CAP_REPLAY_RETIRE == (1ULL << 8),
               "replay capability");
_Static_assert(OBMM_ASYNC_LOAD_START_REPLAY_RETIRE == 1,
               "replay start flag");
_Static_assert(sizeof(struct obmm_async_load_context_v2) == 832, "context size");
_Static_assert(offsetof(struct obmm_async_load_context_v2, x) == 16, "x offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, sp) == 264, "sp offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, pc) == 272, "pc offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, q) == 288, "q offset");
_Static_assert(offsetof(struct obmm_async_load_context_v2, fpcr) == 800,
               "fpcr offset");
_Static_assert(sizeof(struct obmm_async_load_caps_v2) == 64, "caps size");
_Static_assert(sizeof(struct obmm_async_load_map_register_v1) == 64, "map size");
_Static_assert(sizeof(struct obmm_async_load_start_v2) == 40, "start size");
_Static_assert(sizeof(struct obmm_async_load_event_v2) == 72, "event size");
_Static_assert(sizeof(struct obmm_async_load_stats_v2) == 152, "stats size");
_Static_assert(sizeof(struct obmm_async_load_observability_v2) == 144,
               "observability size");
_Static_assert(sizeof(struct obmm_async_load_replay_stats_v1) == 32,
               "replay stats size");
_Static_assert(offsetof(struct obmm_async_load_start_v2, upcall_entry) == 24,
               "upcall entry offset");
_Static_assert(offsetof(struct obmm_async_load_event_v2, interrupted_pc) == 24,
               "interrupted PC offset");
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
    assert "ub_async_load_scheduler_command" in device
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
    assert "gen_helper_async_load_resume" in translate
    assert "HELPER(async_load_remote_load)" in helper
    assert "UB_ASYNC_LOAD_TRY_REPLAYED" in helper
    assert "async_load_replay_valid = true" in helper
    assert "env->pc = upcall_entry" in helper
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
    assert "OBMM_ASYNC_LOAD_IOCTL_GET_EVENT" in runtime
    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" in runtime
    assert "OBMM_COROUTINE_SCHEDULER_ERROR_STAGE_SCHEDULER_ENTER" in runtime
    assert "stp x0, x1, [sp, #16]" in assembly
    assert "stp q30, q31, [sp, #768]" in assembly
    assert ".inst 0xd44a6860" in assembly
    assert "OBMM_ASYNC_LOAD_REG_UPCALL_ENTRY" in driver
    assert "OBMM_ASYNC_LOAD_REG_EVENT_COMMAND" in driver
    assert "OBMM_ASYNC_LOAD_REG_SCHEDULER_COMMAND" in driver
    assert "struct obmm_async_load_start_v2" in driver
    assert "linqu_async_load_create_context" not in driver
    assert "!(event.flags & OBMM_ASYNC_LOAD_EVENT_GET_WAIT)" in driver
    wait_event = driver.split("static long linqu_async_load_get_event", 1)[1].split(
        "static long linqu_async_load_context_commit", 1
    )[0]
    assert "timeout_ns = OBMM_ASYNC_LOAD_MAX_LOAD_TIMEOUT_NS;" in wait_event
    assert "timeout_ns = ctx->load_timeout_ns" not in wait_event
    assert "OBMM_ASYNC_LOAD_STATUS_EVENT_PENDING |" in wait_event
    assert "OBMM_ASYNC_LOAD_STATUS_EVENT_DELIVERED" in wait_event
    assert "linqu_async_load: GET_EVENT timeout" in wait_event
    assert "if (!(status & OBMM_ASYNC_LOAD_STATUS_EVENT_DELIVERED))" in wait_event
    event_command = device.split("static void ub_async_load_event_command", 1)[1].split(
        "bool ub_async_load_device_write", 1
    )[0]
    assert "!state->session_active || !state->upcall_active" not in event_command
    assert "!state->session_active || state->delivered_event_valid" in event_command
    scheduler_command = device.split(
        "static void ub_async_load_scheduler_command", 1
    )[1].split("bool ub_async_load_device_write", 1)[0]
    assert "state->active_context_id = 0" in scheduler_command
    assert "state->upcall_active = true" in scheduler_command
    schedule_after_exit = runtime.split(
        "void obmm_coroutine_scheduler_schedule_after_exit", 1
    )[1].split("static int obmm_coroutine_scheduler_collect_metrics", 1)[0]
    assert schedule_after_exit.index("OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER") < (
        schedule_after_exit.index("obmm_coroutine_scheduler_schedule(runtime")
    )


def test_scheduler_enter_ioctl_is_part_of_guest_abi_v2():
    uapi = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async_load.h"
    ).read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()

    assert "OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER" in uapi
    assert "static long linqu_async_load_scheduler_enter" in driver
    assert "case OBMM_ASYNC_LOAD_IOCTL_SCHEDULER_ENTER:" in driver
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


def test_kernel_artifact_signature_tracks_async_load_v2_sources():
    builder = GUEST_ARTIFACT_BUILDER.read_text()
    signature = builder.split("current_kernel_artifact_signature()", 1)[1].split(
        "kernel_image_stamp_matches()", 1
    )[0]

    assert "include/linux/obmm.h" in signature
    assert "include/uapi/ub/obmm_async_load.h" in signature


class ObmmAsyncLoadCoroutineContractTests(unittest.TestCase):
    def test_uapi_layout(self):
        test_async_load_uapi_v2_layout_compiles_for_aarch64()

    def test_cross_compile(self):
        test_async_load_library_and_shared_cli_cross_compile_without_warnings()

    def test_ordinary_load_data_plane(self):
        test_async_load_data_plane_is_an_ordinary_scalar_load()

    def test_qemu_mechanism_boundary(self):
        test_qemu_provides_mechanism_but_not_coroutine_policy()

    def test_guest_el0_scheduler_ownership(self):
        test_guest_el0_runtime_owns_save_state_and_selection()

    def test_scheduler_enter_ioctl(self):
        test_scheduler_enter_ioctl_is_part_of_guest_abi_v2()

    def test_async_load_producer_consumer_causal_evidence(self):
        test_async_load_producer_consumer_has_causal_upcall_evidence()

    def test_transport_neutrality(self):
        test_async_load_public_contract_is_transport_neutral()

    def test_scenario_contract(self):
        test_async_load_scenarios_do_not_model_qemu_scheduler_cycles()

    def test_kernel_artifact_signature(self):
        test_kernel_artifact_signature_tracks_async_load_v2_sources()


if __name__ == "__main__":
    unittest.main()
