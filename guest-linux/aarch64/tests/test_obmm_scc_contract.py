import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
KERNEL_ROOT = ROOT.parent / "kernel_ub"
QEMU_ROOT = REPO_ROOT / "vendor" / "qemu_8.2.0_ub"
LIB_DIR = ROOT / "libs" / "obmm_scc"
ASYNC_LIB_DIR = ROOT / "libs" / "obmm_async"
APP_DIR = ROOT / "apps" / "obmm_async_coroutine"
GUEST_ARTIFACT_BUILDER = ROOT / "scripts" / "build_guest_artifacts.sh"


def test_scc_uapi_v2_layout_compiles_for_aarch64():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    source = r"""
#include <stddef.h>
#include <ub/obmm_scc.h>
_Static_assert(OBMM_SCC_ABI_VERSION == 2, "ABI version");
_Static_assert(OBMM_SCC_RESUME_HLT_IMM == 0x5343, "resume immediate");
_Static_assert(sizeof(struct obmm_scc_context_v2) == 832, "context size");
_Static_assert(offsetof(struct obmm_scc_context_v2, x) == 16, "x offset");
_Static_assert(offsetof(struct obmm_scc_context_v2, sp) == 264, "sp offset");
_Static_assert(offsetof(struct obmm_scc_context_v2, pc) == 272, "pc offset");
_Static_assert(offsetof(struct obmm_scc_context_v2, q) == 288, "q offset");
_Static_assert(offsetof(struct obmm_scc_context_v2, fpcr) == 800,
               "fpcr offset");
_Static_assert(sizeof(struct obmm_scc_caps_v2) == 64, "caps size");
_Static_assert(sizeof(struct obmm_scc_map_register_v1) == 64, "map size");
_Static_assert(sizeof(struct obmm_scc_start_v2) == 40, "start size");
_Static_assert(sizeof(struct obmm_scc_event_v2) == 72, "event size");
_Static_assert(sizeof(struct obmm_scc_stats_v2) == 152, "stats size");
_Static_assert(sizeof(struct obmm_scc_observability_v2) == 144,
               "observability size");
_Static_assert(offsetof(struct obmm_scc_start_v2, upcall_entry) == 24,
               "upcall entry offset");
_Static_assert(offsetof(struct obmm_scc_event_v2, interrupted_pc) == 24,
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


def test_scc_library_and_shared_cli_cross_compile_without_warnings():
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
                "-idirafter",
                str(KERNEL_ROOT / "include" / "uapi"),
                str(APP_DIR / "obmm_async_coroutine.c"),
                str(LIB_DIR / "obmm_scc.c"),
                str(LIB_DIR / "obmm_scc_aarch64.S"),
                str(ASYNC_LIB_DIR / "obmm_async.c"),
                str(ASYNC_LIB_DIR / "obmm_async_aarch64.S"),
                str(APP_DIR / "uffd_mode.c"),
                str(APP_DIR / "uffd_state.c"),
                str(ROOT / "common" / "obmm_uffd.c"),
                "-pthread",
                "-o",
                str(output),
            ],
            check=True,
        )
        assert output.read_bytes()[:4] == b"\x7fELF"


def test_scheduler_core_data_plane_is_an_ordinary_scalar_load():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    worker = app.split("static void async_scc_worker_entry", 1)[1].split(
        "static int async_compare_u64", 1
    )[0]
    scalar_load = app.split(
        "static __attribute__((noinline)) uint64_t async_scc_scalar_load", 1
    )[1].split("static void async_scc_worker_entry", 1)[0]

    assert "async_scc_scalar_load(address" in worker
    assert worker.count("async_worker_now_ns(worker)") == 4
    assert "obmm_load_submit" not in worker
    assert "obmm_await" not in worker
    for scalar_type in ("uint8_t", "uint16_t", "uint32_t", "uint64_t"):
        assert f"const volatile {scalar_type} *" in scalar_load


def test_qemu_provides_mechanism_but_not_coroutine_policy():
    model_header = (QEMU_ROOT / "include" / "hw" / "ub" / "ub_scc.h").read_text()
    model = (QEMU_ROOT / "hw" / "ub" / "ub_scc.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_scc_device.c").read_text()
    translate = (QEMU_ROOT / "target" / "arm" / "tcg" / "translate-a64.c").read_text()
    helper = (QEMU_ROOT / "target" / "arm" / "tcg" / "helper-a64.c").read_text()

    assert "ObmmSccEvent" in model_header
    assert "ObmmSccArchState" not in model_header
    assert "obmm_scc_schedule_next" not in model_header
    assert "obmm_scc_context_create" not in model_header
    assert "obmm_scc_event_pop" in model
    assert "ub_scc_cpu_take_upcall" in device
    assert "ub_scc_cpu_resume" in device
    assert "active_context_id = context_id" in device
    assert "active_context_id && !state->upcall_active" not in device
    assert "ready_queue" not in device
    assert "OBMM_SCC_RESUME_IMM" in translate
    assert "gen_helper_obmm_scc_resume" in translate
    assert "HELPER(obmm_scc_remote_load)" in helper
    assert "env->pc = upcall_entry" in helper
    assert "obmm_scc_probe_access_range" in helper
    assert "probe_access(env, address, OBMM_SCC_CONTEXT_BYTES" not in helper
    assert "obmm_scc_context_load" in helper
    assert "obmm_scc_context_install" in helper


def test_guest_el0_runtime_owns_save_state_and_selection():
    runtime = (LIB_DIR / "obmm_scc.c").read_text()
    dispatch = runtime.split("void obmm_scc_upcall_dispatch", 1)[1].split(
        "void obmm_scc_context_entry_c", 1
    )[0]
    assembly = (LIB_DIR / "obmm_scc_aarch64.S").read_text()
    driver = (ROOT / "driver" / "linqu_ub_drv.c").read_text()
    device = (QEMU_ROOT / "hw" / "ub" / "ub_scc_device.c").read_text()

    assert "OBMM_SCC_CONTEXT_READY" in runtime
    assert "OBMM_SCC_CONTEXT_WAIT_REMOTE" in runtime
    assert "obmm_scc_choose_ready" in runtime
    assert "obmm_scc_process_event" in runtime
    assert "target->context.x[event->rt] = event->value" in runtime
    assert "target->context.pc = event->fault_pc + 4" in runtime
    assert "runtime->current->state != OBMM_SCC_CONTEXT_DONE" in dispatch
    assert "interrupted_was_running" in dispatch
    assert "OBMM_SCC_IOCTL_GET_EVENT" in runtime
    assert "stp x0, x1, [sp, #16]" in assembly
    assert "stp q30, q31, [sp, #768]" in assembly
    assert ".inst 0xd44a6860" in assembly
    assert "OBMM_SCC_REG_UPCALL_ENTRY" in driver
    assert "OBMM_SCC_REG_EVENT_COMMAND" in driver
    assert "struct obmm_scc_start_v2" in driver
    assert "linqu_scc_create_context" not in driver
    assert "!(event.flags & OBMM_SCC_EVENT_GET_WAIT)" in driver
    event_command = device.split("static void ub_scc_event_command", 1)[1].split(
        "bool ub_scc_device_write", 1
    )[0]
    assert "!state->session_active || !state->upcall_active" not in event_command
    assert "!state->session_active || state->delivered_event_valid" in event_command


def test_p2b_producer_consumer_has_causal_upcall_evidence():
    public = (LIB_DIR / "obmm_scc.h").read_text()
    runtime = (LIB_DIR / "obmm_scc.c").read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()

    assert "struct obmm_scc_trace_event" in public
    assert "obmm_scc_trace_fn trace" in public
    for event in (
        "OBMM_SCC_TRACE_UPCALL_PENDING",
        "OBMM_SCC_TRACE_UPCALL_COMPLETE",
        "OBMM_SCC_TRACE_CONTEXT_RESUME",
    ):
        assert event in runtime
    assert "--p2b-producer-consumer" in app
    assert "async_run_p2b_producer" in app
    assert "async_run_p2b_consumer" in app
    assert "OBMM_P2B_WRITE schema=1" in app
    assert "OBMM_P2B_LDR schema=1 event=issue" in app
    assert "OBMM_P2B_UPCALL schema=1 event=pending" in app
    assert "OBMM_P2B_UPCALL schema=1 event=complete" in app
    assert "OBMM_P2B_COROUTINE_SUMMARY schema=1" in app
    assert "OBMM_P2B_SUMMARY schema=1" in app
    assert "P2B coroutine $coroutine_id causal event order is invalid" in runner
    assert "OBMM_P2B_CAUSAL_SUMMARY" in runner
    assert "blocked load switching to another coroutine" in runner
    assert "source_export_mem_id" in runner


def test_scc_public_contract_is_transport_neutral():
    public_text = (LIB_DIR / "obmm_scc.h").read_text().lower()
    uapi_text = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_scc.h"
    ).read_text().lower()

    for forbidden in ("sim_dec", "urma", "rdma", "roce", "tcp", "cuda"):
        assert forbidden not in public_text
        assert forbidden not in uapi_text


def test_scc_scenarios_do_not_model_qemu_scheduler_cycles():
    for scenario in (REPO_ROOT / "scenarios").glob("mvp_*host_*.yaml"):
        text = scenario.read_text()
        assert "scheduler_core_model:" in text
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


def test_kernel_artifact_signature_tracks_scc_v2_sources():
    builder = GUEST_ARTIFACT_BUILDER.read_text()
    signature = builder.split("current_kernel_artifact_signature()", 1)[1].split(
        "kernel_image_stamp_matches()", 1
    )[0]

    assert "include/linux/obmm.h" in signature
    assert "include/uapi/ub/obmm_scc.h" in signature


class ObmmSccContractTests(unittest.TestCase):
    def test_uapi_layout(self):
        test_scc_uapi_v2_layout_compiles_for_aarch64()

    def test_cross_compile(self):
        test_scc_library_and_shared_cli_cross_compile_without_warnings()

    def test_ordinary_load_data_plane(self):
        test_scheduler_core_data_plane_is_an_ordinary_scalar_load()

    def test_qemu_mechanism_boundary(self):
        test_qemu_provides_mechanism_but_not_coroutine_policy()

    def test_guest_el0_scheduler_ownership(self):
        test_guest_el0_runtime_owns_save_state_and_selection()

    def test_p2b_producer_consumer_causal_evidence(self):
        test_p2b_producer_consumer_has_causal_upcall_evidence()

    def test_transport_neutrality(self):
        test_scc_public_contract_is_transport_neutral()

    def test_scenario_contract(self):
        test_scc_scenarios_do_not_model_qemu_scheduler_cycles()

    def test_kernel_artifact_signature(self):
        test_kernel_artifact_signature_tracks_scc_v2_sources()


if __name__ == "__main__":
    unittest.main()
