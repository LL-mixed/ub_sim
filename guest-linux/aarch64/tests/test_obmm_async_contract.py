import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
KERNEL_ROOT = ROOT.parent / "kernel_ub"
QEMU_ROOT = REPO_ROOT / "vendor" / "qemu_8.2.0_ub"
LIB_DIR = ROOT / "libs" / "obmm_async"
SCC_LIB_DIR = ROOT / "libs" / "obmm_scc"
APP_DIR = ROOT / "apps" / "obmm_async_coroutine"


def test_uapi_layout_compiles_for_aarch64():
    compiler = shutil.which("aarch64-linux-gnu-gcc")
    if not compiler:
        return
    source = r"""
#include <stddef.h>
#include <ub/obmm_async.h>
_Static_assert(sizeof(struct obmm_async_sq_entry_v1) == 64, "SQ size");
_Static_assert(sizeof(struct obmm_async_cq_entry_v1) == 64, "CQ size");
_Static_assert(offsetof(struct obmm_async_sq_entry_v1, token) == 8,
               "SQ token offset");
_Static_assert(offsetof(struct obmm_async_sq_entry_v1, user_data) == 56,
               "SQ user_data offset");
_Static_assert(offsetof(struct obmm_async_cq_entry_v1, token) == 8,
               "CQ token offset");
_Static_assert(offsetof(struct obmm_async_cq_entry_v1, reserved) == 56,
               "CQ reserved offset");
_Static_assert(sizeof(struct obmm_async_observability_v1) == 168,
               "observability size");
_Static_assert(offsetof(struct obmm_async_observability_v1,
                        model_service_ns) == 8,
               "model service offset");
_Static_assert(offsetof(struct obmm_async_observability_v1,
                        backend_sink_copy_ns) == 160,
               "sink copy offset");
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


def test_library_and_cli_cross_compile_without_warnings():
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
                "-static",
                "-I",
                str(LIB_DIR),
                "-I",
                str(SCC_LIB_DIR),
                "-I",
                str(ROOT / "common"),
                "-idirafter",
                str(KERNEL_ROOT / "include" / "uapi"),
                str(APP_DIR / "obmm_async_coroutine.c"),
                str(LIB_DIR / "obmm_async.c"),
                str(LIB_DIR / "obmm_async_aarch64.S"),
                str(SCC_LIB_DIR / "obmm_scc.c"),
                str(SCC_LIB_DIR / "obmm_scc_aarch64.S"),
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


def test_token_wire_golden_vector():
    generation = 0x89ABCDEF
    queue_id = 0x2345
    slot = 0x003F
    token = (generation << 32) | (queue_id << 16) | slot

    assert token == 0x89ABCDEF2345003F
    assert token >> 32 == generation
    assert (token >> 16) & 0xFFFF == queue_id
    assert token & 0xFFFF == slot


def test_context_switch_preserves_frozen_aapcs64_state():
    assembly = (LIB_DIR / "obmm_async_aarch64.S").read_text()

    for register in ("x19", "x20", "x29", "x30", "q8", "q15"):
        assert register in assembly
    assert "mrs x9, fpcr" in assembly
    assert "mrs x9, fpsr" in assembly
    assert "msr fpcr, x9" in assembly
    assert "msr fpsr, x9" in assembly
    assert "mov sp, x9" in assembly
    assert ".note.GNU-stack" in assembly


def test_public_api_and_uapi_are_transport_neutral():
    public_text = (LIB_DIR / "obmm_async.h").read_text().lower()
    uapi_text = (
        KERNEL_ROOT / "include" / "uapi" / "ub" / "obmm_async.h"
    ).read_text().lower()

    for forbidden in ("sim_dec", "urma", "rdma", "roce", "tcp", "cuda"):
        assert forbidden not in public_text
        assert forbidden not in uapi_text


def test_qemu_endpoint_is_routed_and_uses_registered_buffers():
    ubc = (QEMU_ROOT / "hw" / "ub" / "ub_ubc.c").read_text()
    endpoint = (QEMU_ROOT / "hw" / "ub" / "ub_obmm_async.c").read_text()
    link_header = (QEMU_ROOT / "include" / "hw" / "ub" / "ub_link.h").read_text()
    link = (QEMU_ROOT / "hw" / "ub" / "ub_link.c").read_text()

    assert "ub_obmm_async_decode(addr, &obmm_async_reg)" in ubc
    assert "ub_obmm_async_new(" in ubc
    assert "ub_obmm_async_free(" in ubc
    assert ".max_access_size = sizeof(uint64_t)" in ubc
    assert '"node%u-generation%" PRIu64 ".ini"' in ubc
    assert ubc.index("obmm_export_register(record);") < ubc.index(
        "g_rename(tmp_path, path)"
    )
    assert "dma_memory_write(" in endpoint
    assert "OBMM_ASYNC_REG_GUEST_MONOTONIC_NS" in endpoint
    assert "OBMM_ASYNC_REG_OBSERVABILITY_BASE" in endpoint
    assert "ub_obmm_remote_model_reset_stats" in endpoint
    assert "obmm_remote_backend_reset_stats" in endpoint
    assert "obmm_remote_retire_map(" in endpoint
    assert ".map_id = map->resolved.map_id" in endpoint
    assert ".map_generation = map->resolved.map_generation" in endpoint
    assert "QemuMutex tx_lock;" in link_header
    assert '#include "qemu/lockable.h"' in link
    assert "qemu_mutex_init(&s->tx_lock);" in link
    assert "qemu_mutex_destroy(&s->tx_lock);" in link
    write_start = link.index("int ub_link_write_message(")
    write_end = link.index("static void ub_link_reopen_after_write_error", write_start)
    write_body = link[write_start:write_end]
    assert write_body.index("QEMU_LOCK_GUARD(&s->tx_lock);") < write_body.index(
        "ub_link_shm_write_message(s, buf, len, errp)"
    )
    assert "aio_poll(" not in link[
        link.index("static int ub_link_shm_write_message("):write_end
    ]
    bounded_start = link.rindex("static int ub_link_write_all_bounded(")
    bounded_end = link.index("static bool ub_link_ensure_rx_capacity", bounded_start)
    assert "aio_poll(" not in link[bounded_start:bounded_end]


def test_sync_scalar_and_async_modes_share_canonical_remote_identity():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    logical = (APP_DIR / "logical_op.h").read_text()

    assert "remote_map && measurement && app->config.access_bytes <= 8" in app
    assert "async_scc_scalar_load(" in app
    assert "sigsetjmp(async_sync_fault_environment" in app
    assert "async_sync_fault_handler" in app
    assert "A modeled error must fail closed" in app
    assert "app->failures++;" in app[
        app.index("A modeled error must fail closed") :
        app.index("memcpy(app->buffers[0].data, &value")
    ]
    assert "--expected-outcome" in app
    assert "obmm_logical_remote_ordinal(" in app
    assert "remote_local_ordinal * coroutine_count + coroutine_id" in logical
    assert "switch_ns_p50=0" not in app
    assert "cq_drain_ns_p50=0" not in app
    for metric in (
        "submit_ns_p50",
        "switch_ns_p50",
        "cq_drain_ns_p50",
        "ready_ns",
        "wait_ns",
        "idle_ns",
        "no_ready",
    ):
        assert metric in app


def test_build_run_and_launcher_contracts():
    builder = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    guest_builder = (ROOT / "scripts" / "build_guest_artifacts.sh").read_text()
    common = (ROOT / "scripts" / "qemu_ub_common.sh").read_text()
    qemu_builder = (ROOT / "scripts" / "build_qemu_binary.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    launcher = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()
    eval_runner = (ROOT / "scripts" / "run_ub_obmm_eval.sh").read_text()
    four_node_launcher = (
        ROOT / "scripts" / "launch_ub_four_node_headless.sh"
    ).read_text()
    eight_node_launcher = (
        ROOT / "scripts" / "launch_ub_eight_node_headless.sh"
    ).read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()

    assert 'OBMM_ASYNC_COROUTINE_BIN="$OUT_DIR/obmm_async_coroutine"' in builder
    assert "OBMM_ASYNC_COROUTINE_SCC_ASM_SRC" in builder
    assert '"$INITRAMFS_DIR/bin/obmm_async_coroutine"' in builder
    assert "zsh ./scripts/build_initramfs.sh" in guest_builder
    assert "zsh ./scripts/build_guest_artifacts.sh" in common
    assert "zsh ./scripts/build_qemu_binary.sh" in common
    assert '"${UB_USE_PREBUILT_QEMU:-0}" == "1"' in common
    assert "using verified prebuilt QEMU binary" in common
    assert common.index('"${UB_USE_PREBUILT_QEMU:-0}" == "1"') < common.index(
        "zsh ./scripts/build_qemu_binary.sh"
    )
    assert "linqu_obmm_async_coroutine=1" in run_app
    assert "run_obmm_async_coroutine" in run_app
    assert "--remote-memory-model-manifest" in launcher
    assert "--obmm-async-args" in launcher
    assert "ubc.remote-memory-model-manifest=" in launcher
    assert "--node-count 2|4|8" in eval_runner
    assert "launch_ub_four_node_headless.sh" in eval_runner
    assert "launch_ub_eight_node_headless.sh" in eval_runner
    assert 'launcher_output="$(zsh "$LAUNCHER")"' in eval_runner
    assert 'zsh "$CLEANUP_SCRIPT"' in eval_runner
    assert "OBMM_NODE_EVIDENCE" in eval_runner
    assert "OBMM_RUN_EVIDENCE" in eval_runner
    assert "scenario_sha256=" in eval_runner
    assert "model_file_sha256=" in eval_runner
    assert "qemu_sha256=" in eval_runner
    assert "kernel_sha256=" in eval_runner
    assert "initramfs_sha256=" in eval_runner
    assert "obmm_async_trace_sample_ppm" in eval_runner
    assert "obmm_async_expected_outcome" in eval_runner
    assert "obmm_async_expected_outcome" in run_app
    assert "obmm_async_p2b_producer_consumer" in run_app
    assert "obmm_async_p2b_completion" in run_app
    assert "obmm_async_producer_index" in run_app
    assert "--p2b-completion" in eval_runner
    assert "P2B replay mode lacks exact-once retirement evidence" in eval_runner
    assert "OBMM_OPERATION_TRACE" in eval_runner
    assert "OBMM_P2B_NODE_EVIDENCE" in eval_runner
    assert "--p2b-producer-consumer" in eval_runner
    assert "cross-node summary mismatch" in eval_runner
    assert "obmm.mempool_size=512M" in eval_runner
    assert "cma=64M" in eval_runner
    assert "tr -d '\\r'" in eval_runner
    assert "mktemp -d" in eval_runner
    assert 'export UB_FM_SHARED_DIR="$OBMM_RUN_SHARED_DIR"' in eval_runner
    assert "logappend=off" in (
        ROOT / "scripts" / "launch_ub_eight_node_headless.sh"
    ).read_text()
    assert "logappend=off" in (
        ROOT / "scripts" / "launch_ub_four_node_headless.sh"
    ).read_text()
    assert "successful outcome did not complete exactly once" in eval_runner
    assert "cleanup left QEMU running" in eval_runner
    assert ') >>"$CONTROL_LOG" 2>&1 &!' in four_node_launcher
    for headless_launcher in (four_node_launcher, eight_node_launcher):
        assert "ubc.remote-memory-model-manifest=" in headless_launcher
        assert "ubc.scheduler-core-model=" in headless_launcher
        assert "export QEMU_BIN=" in headless_launcher
        assert "export KERNEL_IMAGE=" in headless_launcher
        assert "export INITRAMFS_IMAGE=" in headless_launcher
    assert "OBMM_ASYNC_SUMMARY abi=%u" in app
    assert "OBMM_ASYNC_SELFTEST abi=%u" in app
    assert "OBMM_APP_ERROR schema=1 stage=%s" in app
    assert "OBMM_VERIFY_FAILURE schema=1 mode=%s ordinal=%" in app
    assert "byte_index=%u expected=%02x" in app
    assert "--with-obmm-tests" in qemu_builder
    for target in (
        "tests/unit/test-ub-obmm-remote",
        "tests/unit/test-ub-obmm-remote-model",
        "tests/unit/test-ub-scc",
    ):
        assert target in qemu_builder


def test_diagnostic_trace_excludes_warmup_operations():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()

    for function in (
        "async_run_split_phase_with_warmup",
        "async_run_uffd_with_warmup",
        "async_run_scc_with_warmup",
    ):
        body = app.split(f"static int {function}", 1)[1].split("\n}\n", 1)[0]
        assert "uint32_t trace_sample_ppm = app->config.trace_sample_ppm;" in body
        disable = body.index("app->config.trace_sample_ppm = 0;")
        restore = body.index("app->config.trace_sample_ppm = trace_sample_ppm;")
        reset = body.index("async_reset_workload_state(app);")
        assert disable < restore < reset


def test_diagnostic_trace_does_not_block_inside_preemptible_coroutine():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()
    record = app.split("static void async_trace_operation", 1)[1].split(
        "\n}\n", 1
    )[0]
    flush = app.split("static void async_flush_operation_trace", 1)[1].split(
        "\n}\n", 1
    )[0]

    assert "atomic_fetch_add_explicit" in record
    assert "pthread_mutex_lock" not in record
    assert "printf(" not in record
    assert "OBMM_OPERATION_TRACE schema=1" in flush


def test_launcher_and_run_app_syntax():
    zsh = shutil.which("zsh")
    if not zsh:
        return
    subprocess.run(
        [zsh, "-n", str(ROOT / "scripts" / "build_qemu_binary.sh")],
        check=True,
    )
    subprocess.run(
        [zsh, "-n", str(ROOT / "scripts" / "run_ub_dual_node_apps.sh")],
        check=True,
    )
    for script in (
        "build_guest_artifacts.sh",
        "run_ub_obmm_eval.sh",
        "launch_ub_four_node_headless.sh",
        "launch_ub_eight_node_headless.sh",
    ):
        subprocess.run(
            [zsh, "-n", str(ROOT / "scripts" / script)],
            check=True,
        )
    subprocess.run(
        ["sh", "-n", str(ROOT / "initramfs" / "run_app")],
        check=True,
    )


def test_eval_runner_fails_closed_before_launch():
    runner = ROOT / "scripts" / "run_ub_obmm_eval.sh"
    scenario = REPO_ROOT / "scenarios" / "mvp_2host_single_domain.yaml"
    zsh = shutil.which("zsh")
    if not zsh:
        return
    with tempfile.TemporaryDirectory() as directory:
        model = Path(directory) / "model.json"
        model.write_text("{}\n")
        common = [
            zsh,
            str(runner),
            "--node-count",
            "2",
            "--scenario-config",
            str(scenario),
            "--remote-memory-model-manifest",
            str(model),
        ]
        unknown = subprocess.run(
            common + ["--obmm-async-args", "--unknown value"],
            text=True,
            capture_output=True,
        )
        assert unknown.returncode == 2
        assert "unsupported --obmm-async-args option" in unknown.stderr

        missing_scheduler = subprocess.run(
            common
            + [
                "--obmm-async-args",
                "--mode scheduler-core --iterations 1 --verify",
            ],
            text=True,
            capture_output=True,
        )
        assert missing_scheduler.returncode == 2
        assert "requires --scheduler-core-model" in missing_scheduler.stderr

        wrong_producer = subprocess.run(
            common
            + [
                "--scheduler-core-model",
                "v2|enabled=1|contexts=64|pending=64|events=128|clock_mhz=1000",
                "--obmm-async-args",
                "--mode scheduler-core --p2b-producer-consumer "
                "--producer-index 1 --coroutines 2 --iterations 2 "
                "--access-bytes 8 --pattern sequential --warmup 0 --verify",
            ],
            text=True,
            capture_output=True,
        )
        assert wrong_producer.returncode == 2
        assert "producer_index=0" in wrong_producer.stderr


class ObmmAsyncContractTests(unittest.TestCase):
    def test_uapi_layout(self):
        test_uapi_layout_compiles_for_aarch64()

    def test_cross_compile(self):
        test_library_and_cli_cross_compile_without_warnings()

    def test_token_wire(self):
        test_token_wire_golden_vector()

    def test_context_switch_state(self):
        test_context_switch_preserves_frozen_aapcs64_state()

    def test_transport_neutrality(self):
        test_public_api_and_uapi_are_transport_neutral()

    def test_qemu_endpoint(self):
        test_qemu_endpoint_is_routed_and_uses_registered_buffers()

    def test_canonical_remote_identity(self):
        test_sync_scalar_and_async_modes_share_canonical_remote_identity()

    def test_build_run_launcher(self):
        test_build_run_and_launcher_contracts()

    def test_script_syntax(self):
        test_launcher_and_run_app_syntax()

    def test_eval_runner_fail_closed(self):
        test_eval_runner_fails_closed_before_launch()


if __name__ == "__main__":
    unittest.main()
