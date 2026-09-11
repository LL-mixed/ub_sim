import pathlib
import platform
import shutil
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST = ROOT / "guest-linux" / "aarch64"
LIB = GUEST / "libs" / "gva_manager"


def stage_uapi(directory):
    include = directory / "include"
    shutil.copytree(
        ROOT / "guest-linux" / "kernel_ub" / "include" / "uapi" / "ub",
        include / "ub", dirs_exist_ok=True,
    )
    return include


def compile_golden(compiler, output):
    include = stage_uapi(output.parent)
    subprocess.run(
        [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         "-I", str(LIB), "-I", str(include),
         str(GUEST / "tests" / "gva_manager_golden.c"),
         str(LIB / "gva_manager.c"), "-o", str(output)],
        check=True,
    )


class GvaManagerLibraryTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_bootstrap_runner_rejects_invalid_config(self):
        runner = GUEST / "scripts" / "run_ub_dual_node_gsva_manager_bootstrap.sh"
        with tempfile.TemporaryDirectory() as directory:
            for args in ([str(pathlib.Path(directory) / "missing.env")], ["a", "b"]):
                with self.subTest(args=args):
                    result = subprocess.run(
                        ["zsh", str(runner), *args], capture_output=True,
                        text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertNotIn("starting nodeA", result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_creates_pid_directory_with_external_artifacts(self):
        with tempfile.TemporaryDirectory() as directory:
            guest = pathlib.Path(directory) / "guest-linux" / "aarch64"
            scripts = guest / "scripts"
            scripts.mkdir(parents=True)
            runner = scripts / "run_ub_two_node_gva_manager_segment_cli_test.sh"
            shutil.copyfile(GUEST / "scripts" / runner.name, runner)
            # Exercise launch setup without starting QEMU or loading artifacts.
            (scripts / "qemu_ub_common.sh").write_text(
                'ensure_sim_kernel_append_defaults() { printf "%s" "$1"; }\n'
                'ensure_qemu_ub_binary() { printf "/usr/bin/true"; }\n'
                'ensure_ub_guest_artifacts() { :; }\n'
            )
            config = pathlib.Path(directory) / "test.env"
            config.write_text(
                f'UB_FM_SHARED_DIR="{directory}/links"\n'
                'LINK_WAIT_SECS=0\nRUN_ID=directory-test\n'
            )
            result = subprocess.run(
                ["zsh", str(runner), str(config)],
                capture_output=True, text=True, timeout=10,
            )
            self.assertTrue((guest / "out").is_dir())
            self.assertEqual(list((guest / "out").glob("*.pid")), [])
            self.assertEqual(result.returncode, 1)
            self.assertIn("FM links not ready", result.stderr)
            self.assertNotIn("no such file or directory", result.stderr)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_rejects_missing_config_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run(
                ["zsh", str(GUEST / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh"),
                 str(pathlib.Path(directory) / "missing.env")],
                capture_output=True, text=True, timeout=10,
            )
        self.assertEqual(result.returncode, 2)
        self.assertIn("config file not found", result.stderr)
        self.assertNotIn("starting nodeA", result.stdout)

    @unittest.skipUnless(shutil.which("zsh"), "requires zsh")
    def test_runner_rejects_extra_arguments_before_launch(self):
        result = subprocess.run(
            ["zsh", str(GUEST / "scripts" / "run_ub_two_node_gva_manager_segment_cli_test.sh"),
             "one.env", "two.env"], capture_output=True, text=True, timeout=10,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("usage:", result.stderr)

    @unittest.skipUnless(platform.system() == "Linux", "requires Linux UAPI")
    def test_kernel_results_and_failure_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "gva-manager-golden"
            compile_golden("cc", binary)
            result = subprocess.run(
                [str(binary)], check=True, capture_output=True, text=True,
                timeout=10,
            )
        self.assertEqual(result.stdout.strip(), "gva_manager_golden=pass")

    def test_aarch64_compile(self):
        compiler = shutil.which("aarch64-linux-gnu-gcc")
        if not compiler and platform.system() == "Linux" and platform.machine() == "aarch64":
            compiler = shutil.which("cc")
        if not compiler:
            self.skipTest("aarch64 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            compile_golden(compiler, pathlib.Path(directory) / "gva-manager-golden")

    def test_existing_cli_links_library(self):
        native = platform.system() == "Linux"
        compiler = shutil.which("cc" if native else "aarch64-linux-gnu-gcc")
        if not compiler:
            self.skipTest("Linux compiler unavailable")
        kernel = ROOT / "guest-linux" / "kernel_ub"
        vendor = ROOT / "vendor" / "obmm" / "src" / "libobmm"
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "linqu_gva_manager"
            include = stage_uapi(binary.parent)
            subprocess.run(
                [compiler, "-O2", "-Wall", "-Wextra", "-D__EXPORTED_HEADERS__",
                 "-pthread", "-I", str(include), "-I", str(GUEST / "common"),
                 "-I", str(GUEST / "libs" / "obmm_queue"), "-I", str(vendor),
                 "-idirafter", str(kernel / "include" / "uapi"),
                 "-idirafter", str(kernel / "include"),
                 str(GUEST / "apps" / "gva_manager" / "gva_manager.c"),
                 str(LIB / "gva_manager.c"), str(vendor / "libobmm.c"),
                 str(GUEST / "common" / "obmm_vendor_adaptor_sim.c"),
                 "-o", str(binary)],
                check=True, capture_output=True, text=True,
            )
            if native:
                result = subprocess.run(
                    [str(binary), "--invalid-option"], capture_output=True, text=True,
                    timeout=10,
                )
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("gva_manager --alloc", result.stderr)
                self.assertNotIn("open /dev/obmm", result.stdout)


if __name__ == "__main__":
    unittest.main()
