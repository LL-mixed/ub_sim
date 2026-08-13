import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
KERNEL_ROOT = ROOT.parent / "kernel_ub"
APP_DIR = ROOT / "apps" / "obmm_async_coroutine"
ASYNC_LIB_DIR = ROOT / "libs" / "obmm_async"
SCC_LIB_DIR = ROOT / "libs" / "obmm_scc"
COMMON_DIR = ROOT / "common"


def test_page_state_machine_runs_on_host():
    compiler = shutil.which("cc")
    if not compiler:
        return
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / "uffd_state_test"
        subprocess.run(
            [
                compiler,
                "-std=c11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                str(APP_DIR / "uffd_state.c"),
                str(APP_DIR / "test_uffd_state.c"),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


def test_userfaultfd_shared_cli_cross_compiles_without_warnings():
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
                str(ASYNC_LIB_DIR),
                "-I",
                str(SCC_LIB_DIR),
                "-I",
                str(COMMON_DIR),
                "-idirafter",
                str(KERNEL_ROOT / "include" / "uapi"),
                str(APP_DIR / "obmm_async_coroutine.c"),
                str(APP_DIR / "uffd_mode.c"),
                str(APP_DIR / "uffd_state.c"),
                str(COMMON_DIR / "obmm_uffd.c"),
                str(ASYNC_LIB_DIR / "obmm_async.c"),
                str(ASYNC_LIB_DIR / "obmm_async_aarch64.S"),
                str(SCC_LIB_DIR / "obmm_scc.c"),
                str(SCC_LIB_DIR / "obmm_scc_aarch64.S"),
                "-pthread",
                "-o",
                str(output),
            ],
            check=True,
        )
        assert output.read_bytes()[:4] == b"\x7fELF"


def test_only_standard_missing_mode_is_used():
    wrapper = (COMMON_DIR / "obmm_uffd.c").read_text()
    mode = (APP_DIR / "uffd_mode.c").read_text()
    source = wrapper + mode

    assert "UFFD_USER_MODE_ONLY" in wrapper
    assert "UFFDIO_API" in wrapper
    assert "UFFDIO_REGISTER_MODE_MISSING" in wrapper
    assert "UFFD_EVENT_PAGEFAULT" in wrapper
    assert "UFFDIO_COPY" in wrapper
    for forbidden in (
        "UFFDIO_REGISTER_MODE_USWAP",
        "UFFDIO_COPY_MODE_DIRECT_MAP",
        "UFFDIO_ZEROPAGE",
    ):
        assert forbidden not in source


def test_cli_is_mode_strict_and_has_an_independent_summary():
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()

    assert 'strcmp(value, "userfaultfd")' in app
    assert 'strcmp(value, "present-hit")' in app
    assert 'strcmp(value, "missing-remote")' in app
    for option in (
        "--uffd-case",
        "--worker-threads",
        "--handler-cpu",
        "--pages",
    ):
        assert option in app
    assert "config->access_bytes != 4096" in app
    assert "uint32_t p2_only" in app
    assert "OBMM_UFFD_SUMMARY schema=1" in app
    assert "OBMM_UFFD_UNSUPPORTED" in app
    assert "wake_ns_p50" in app
    assert "wake_ns_p99" in app


def test_phase_shutdown_and_failure_contracts_are_explicit():
    mode = (APP_DIR / "uffd_mode.c").read_text()

    assert "madvise(runtime->shadow" in mode
    assert "MADV_DONTNEED" in mode
    assert "obmm_uffd_reset_phase(runtime)" in mode
    assert "obmm_uffd_unregister(&runtime->uffd)" in mode
    assert "obmm_uffd_close(&runtime->uffd)" in mode
    assert "pthread_join(handler, NULL)" in mode
    assert "OBMM_UFFD_FAIL_STOP" in mode
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    assert "fail_closed_process_exit=1" in run_app
    assert "obmm_uffd_poison" in mode


def test_page_lifecycle_trace_obeys_the_deterministic_ppm_gate():
    header = (APP_DIR / "uffd_mode.h").read_text()
    mode = (APP_DIR / "uffd_mode.c").read_text()
    app = (APP_DIR / "obmm_async_coroutine.c").read_text()

    assert "uint32_t trace_sample_ppm;" in header
    assert ".trace_sample_ppm = app->config.trace_sample_ppm" in app
    assert "if (!runtime->config->trace_sample_ppm)" in mode
    assert "runtime->logical_ordinals[page]" in mode
    assert "OBMM_UFFD_TRACE_INITIAL" not in mode
    assert "OBMM_UFFD_TRACE_INTERVAL" not in mode


def test_guest_packaging_and_launcher_forward_all_uffd_options():
    builder = (ROOT / "scripts" / "build_initramfs.sh").read_text()
    run_app = (ROOT / "initramfs" / "run_app").read_text()
    launcher = (ROOT / "scripts" / "run_ub_dual_node_apps.sh").read_text()

    for source_name in (
        "OBMM_ASYNC_COROUTINE_UFFD_MODE_SRC",
        "OBMM_ASYNC_COROUTINE_UFFD_STATE_SRC",
        "OBMM_ASYNC_COROUTINE_UFFD_WRAPPER_SRC",
    ):
        assert source_name in builder
    for key in (
        "obmm_uffd_case",
        "obmm_uffd_worker_threads",
        "obmm_uffd_handler_cpu",
        "obmm_uffd_pages",
    ):
        assert key in run_app
        assert key in launcher
    assert "OBMM_UFFD_SUMMARY schema=1" in launcher


def test_launcher_and_run_app_syntax():
    zsh = shutil.which("zsh")
    if not zsh:
        return
    subprocess.run(
        [zsh, "-n", str(ROOT / "scripts" / "run_ub_dual_node_apps.sh")],
        check=True,
    )
    subprocess.run(
        ["sh", "-n", str(ROOT / "initramfs" / "run_app")],
        check=True,
    )


class ObmmUffdContractTests(unittest.TestCase):
    def test_state_machine(self):
        test_page_state_machine_runs_on_host()

    def test_cross_compile(self):
        test_userfaultfd_shared_cli_cross_compiles_without_warnings()

    def test_standard_uapi_only(self):
        test_only_standard_missing_mode_is_used()

    def test_mode_strict_cli(self):
        test_cli_is_mode_strict_and_has_an_independent_summary()

    def test_failure_and_shutdown(self):
        test_phase_shutdown_and_failure_contracts_are_explicit()

    def test_timed_trace_gate(self):
        test_page_lifecycle_trace_obeys_the_deterministic_ppm_gate()

    def test_packaging_and_launcher(self):
        test_guest_packaging_and_launcher_forward_all_uffd_options()

    def test_script_syntax(self):
        test_launcher_and_run_app_syntax()


if __name__ == "__main__":
    unittest.main()
