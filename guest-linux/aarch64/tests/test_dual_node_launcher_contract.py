from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class DualNodeLauncherContractTest(unittest.TestCase):
    def setUp(self):
        self.launcher = (
            ROOT / "scripts" / "run_ub_dual_node_apps.sh"
        ).read_text()

    def test_watchdog_reaps_its_sleep_process(self):
        self.assertIn('sleep "$timeout_sec" &', self.launcher)
        self.assertIn('sleep_pid=$!', self.launcher)
        self.assertIn('kill "$sleep_pid"', self.launcher)
        self.assertIn('wait "$sleep_pid"', self.launcher)
        self.assertIn('wait "$WATCHDOG_PID"', self.launcher)

    def test_interactive_mode_publishes_run_owned_serial_sockets(self):
        self.assertIn('--interactive-after-pass)', self.launcher)
        self.assertIn(
            'SERIAL_RUNTIME_DIR="/tmp/ubqe_${RUN_ID}"', self.launcher
        )
        self.assertIn(
            'manifest: guest-linux/aarch64/out/dual_node_serial_env.{run_id}.sh',
            (ROOT.parents[1] / "crates" / "sim-console" / "catalog" / "demos.yaml")
            .read_text(),
        )
        self.assertIn('export NODEA_SERIAL_SOCKET=', self.launcher)
        self.assertIn('export NODEB_SERIAL_SOCKET=', self.launcher)
        self.assertIn(
            '-chardev "socket,id=ser0,path=$serial_socket,server=on,wait=off,'
            'logfile=$guest_log,logappend=off"',
            self.launcher,
        )
        self.assertIn('-serial chardev:ser0', self.launcher)

    def test_interactive_mode_retains_guests_only_after_validation(self):
        validation = self.launcher.index(
            'echo "iteration ${iter}: dual-node apps pass"'
        )
        hold = self.launcher.index(
            'echo "interactive shells ready; use node input or Stop to terminate"'
        )

        self.assertLess(validation, hold)
        self.assertIn('if [[ "$INTERACTIVE_AFTER_PASS" -eq 0 ]]; then', self.launcher)
        self.assertIn('cleanup_watchdog', self.launcher[validation:hold])
        self.assertIn('while kill -0', self.launcher[hold:])
        self.assertIn('cleanup_serial_runtime', self.launcher)

    def test_signals_exit_and_leave_cleanup_to_the_exit_trap(self):
        self.assertIn(
            "trap 'cleanup_watchdog; cleanup_all_app_pid_files; "
            "cleanup_serial_runtime; ub_guest_boot_cleanup' EXIT",
            self.launcher,
        )
        self.assertIn("trap 'exit 130' INT", self.launcher)
        self.assertIn("trap 'exit 143' TERM", self.launcher)
        self.assertNotIn("cleanup_serial_runtime' EXIT INT TERM", self.launcher)

    def test_cleanup_keeps_other_runs_and_is_idempotent(self):
        function = "cleanup_all_app_pid_files() {" + self.launcher.split(
            "cleanup_all_app_pid_files() {", 1
        )[1].split("\nwrite_serial_manifest()", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            owned = [out / f"ub_{node}.apps.owned.1.pid" for node in ("nodeA", "nodeB")]
            foreign = [out / f"ub_{node}.apps.foreign.1.pid" for node in ("nodeA", "nodeB")]
            for file in owned + foreign:
                file.write_text("fixture\n")
            subprocess.run(
                ["zsh", "-c", function + '\nOUT_DIR="$1"; RUN_ID=owned; '
                 'cleanup_pid() { rm -f -- "$1"; }; '
                 'cleanup_all_app_pid_files; cleanup_all_app_pid_files',
                 "cleanup-test", str(out)], check=True,
            )
            self.assertTrue(all(not file.exists() for file in owned))
            self.assertTrue(all(file.exists() for file in foreign))

    def test_direct_el0_gate_preserves_the_kernel_task_replay_mode(self):
        function = "validate_obmm_async_coroutine_log() {" + self.launcher.split(
            "validate_obmm_async_coroutine_log() {", 1
        )[1].split("\nvalidate_obmm_import_stress_log()", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "guest.log"
            for args, upcalls, ioctls, passed in (
                ("--mode async-load", 1, 0, True),
                ("--mode async-load", 0, 0, False),
                ("--mode async-load", 1, 1, False),
                ("--mode async-load --kernel-task-replay", 0, 0, True),
            ):
                log.write_text(
                    "OBMM_ASYNC_SUMMARY abi=1 mode=async-load status=pass "
                    f"failures=0 stale=0 direct_el0_upcalls={upcalls} "
                    f"kernel_hotpath_ioctls={ioctls}\n"
                )
                result = subprocess.run(
                    ["zsh", "-c", function + '\nOBMM_ASYNC_ARGS="$2"; '
                     'assert_log_has() { grep -qE "$2" "$1"; }; '
                     'assert_log_absent() { ! grep -qE "$2" "$1"; }; '
                     'validate_obmm_async_coroutine_log nodeA "$1"',
                     "el0-gate-test", str(log), args], check=False,
                )
                self.assertEqual(result.returncode == 0, passed, args)

    def test_managed_interactive_waits_for_both_actual_guest_shells(self):
        self.assertIn('mem_service.object_share_interactive=${INTERACTIVE_AFTER_PASS}',
                      self.launcher)
        start = self.launcher.index('echo "iteration ${iter}: dual-node apps pass"')
        ready = self.launcher.index('echo "interactive shells ready;')
        section = self.launcher[start:ready]
        for node in (0, 1):
            self.assertIn(f'mem_service interactive-shell: ready node={node} '
                          'infrastructure=retained', section)
        self.assertEqual(section.count('"$RUN_SECS" || return 1'), 2)
        self.assertLess(section.index('wait_for_log_pattern'),
                        section.index('cleanup_watchdog'))

    def test_managed_shell_preserves_infrastructure_and_default_wait(self):
        source = (ROOT / 'initramfs/run_app').read_text()
        body = 'mem_service_object_share_wait_infrastructure() {' + source.split(
            'mem_service_object_share_wait_infrastructure() {', 1)[1].split(
            '\nmem_service_object_share_failure_snapshot()', 1)[0]
        self.assertIn('mem_service_object_share_wait_infrastructure "$node" "$worker_pid"',
                      source)
        self.assertNotIn('kill ', body)
        self.assertNotIn('exec ', body)
        # Exercise the real shell branch with only its interactive shell mocked.
        body = body.replace('/bin/busybox sh', 'interactive_shell')
        for mode, code, expected in ((0, 7, 'worker_wait=123'),
                                     (1, 4, 'interactive_shell=entered')):
            with self.subTest(interactive=mode):
                harness = (f'cmdline_value() {{ echo {mode}; }}\n'
                           'log() { echo "$*"; }\n'
                           'wait() { echo "worker_wait=$1"; return 7; }\n'
                           'interactive_shell() { echo interactive_shell=entered; return 4; }\n'
                           + body + '\nmem_service_object_share_wait_infrastructure 0 123')
                result = subprocess.run(['sh', '-c', harness],
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, code, result.stderr)
                self.assertIn(expected, result.stdout)
                self.assertEqual('interactive-shell: ready' in result.stdout, bool(mode))
                self.assertEqual('worker_wait=' in result.stdout, not mode)

    def test_interactive_mode_is_exposed_by_the_launcher_cli(self):
        result = subprocess.run(
            [
                str(ROOT / "scripts" / "run_ub_dual_node_apps.sh"),
                "--help",
            ],
            check=True,
            capture_output=True,
            text=True,
        )

        self.assertIn("--interactive-after-pass", result.stdout)
        self.assertIn("Keep validated guests", result.stdout)

        invalid = subprocess.run(
            [
                str(ROOT / "scripts" / "run_ub_dual_node_apps.sh"),
                "--interactive-after-pass",
                "--iterations",
                "2",
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(2, invalid.returncode)
        self.assertIn("requires exactly one iteration", invalid.stderr)


if __name__ == "__main__":
    unittest.main()
