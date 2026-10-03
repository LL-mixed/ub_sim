#!/usr/bin/env python3

import subprocess
import json
import os
import shutil
import tempfile
import unittest
from pathlib import Path


class BootUbEightNodeShellContractTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(__file__).resolve().parents[3]
        self.script = (
            self.repo
            / "guest-linux"
            / "aarch64"
            / "scripts"
            / "boot_ub_eight_node_shell.sh"
        )

    def test_script_exists_and_is_executable(self):
        self.assertTrue(self.script.is_file())
        self.assertTrue(self.script.stat().st_mode & 0o111)

    def test_zsh_syntax_ok(self):
        result = subprocess.run(
            ["zsh", "-n", str(self.script)],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_help_lists_subcommands(self):
        result = subprocess.run(
            [str(self.script), "--help"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        for token in ("up", "attach", "status", "stop", "nodeA"):
            self.assertIn(token, result.stderr)

    def test_unknown_subcommand_rejected(self):
        result = subprocess.run(
            [str(self.script), "bogus"],
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        self.assertEqual(result.returncode, 2)

    def test_guest_runner_template_is_interactive_shell(self):
        source = self.script.read_text()
        # The guest-side runner must hand the serial console to an interactive
        # root bash (not the W5 workload), reading AND writing ttyAMA0.
        self.assertIn("setsid -c /bin/bash -l </dev/ttyAMA0 >/dev/ttyAMA0", source)
        self.assertIn("chmod +x \"$ENV_DIR/runner.sh\"", source)

    def test_delegates_to_eight_node_launcher_with_openeuler_backend(self):
        source = self.script.read_text()
        self.assertIn("launch_ub_eight_node_headless.sh", source)
        # QEMU runs in a long-lived container (prebuilt binary needs container
        # libs); sockets are bind-mounted to the host for attach.
        self.assertIn("docker run -d", source)
        self.assertIn("UB_USE_PREBUILT_QEMU=1", source)
        self.assertIn("SIM_W5_GUEST_BACKEND=openeuler", source)

    def test_container_receives_host_guest_memory_and_cmdline_as_arguments(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            guest = root / "guest-linux/aarch64"
            scripts = guest / "scripts"
            scripts.mkdir(parents=True)
            (guest / "out/openeuler_guest").mkdir(parents=True)
            (guest / "out/openeuler_guest/rootfs.qcow2").touch()
            shutil.copy(self.script, scripts)
            manifest = guest / "out/fixture.env"
            manifest.write_text("export RUN_ID=fixture\nexport RUN_DIR=/fixture\n")
            stub_dir = root / "stubs"
            stub_dir.mkdir()
            for name, body in (
                ("uname", '#!/bin/sh\ncase "$1" in -s) echo Linux;; -m) echo aarch64;; esac\n'),
                ("pgrep", "#!/bin/sh\nexit 1\n"),
                ("docker", "#!/usr/bin/env python3\n"
                 "import json, os, sys\n"
                 "args = sys.argv[1:]\n"
                 "if args and args[-1].endswith('launch_ub_eight_node_headless.sh'):\n"
                 "    with open(os.environ['FIXTURE_CAPTURE'], 'w') as f: json.dump(args, f)\n"
                 "    print(os.environ['FIXTURE_MANIFEST'])\n"
                 "elif args and 'liburing.so.1 => not found' in args[-1]:\n"
                 "    sys.exit(1)\n"),
            ):
                stub = stub_dir / name
                stub.write_text(body)
                stub.chmod(0o755)
            capture = root / "docker-args.json"
            env = dict(os.environ, PATH=str(stub_dir) + ":" + os.environ["PATH"],
                       FIXTURE_CAPTURE=str(capture), FIXTURE_MANIFEST=str(manifest),
                       SIM_W5_OPENEULER_GUEST_DIR=str(guest / "out/openeuler_guest"),
                       UB_FM_SHARED_DIR=str(root / "shared"), QEMU_MEM="6G", QEMU_SMP="3",
                       APPEND_EXTRA="pmd_mapping=100% obmm.mempool_size=0")
            result = subprocess.run(["zsh", str(scripts / self.script.name), "up"],
                                    capture_output=True, text=True, env=env, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            args = json.loads(capture.read_text())
            for expected in ("QEMU_MEM=6G", "QEMU_SMP=3", "APPEND_EXTRA=" + env["APPEND_EXTRA"]):
                self.assertIn(expected, args)
            self.assertNotIn("-c", args)


if __name__ == "__main__":
    unittest.main()
