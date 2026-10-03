"""Shared guest OS CLI and boot preparation, without launching QEMU."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


REPO = Path(__file__).resolve().parents[3]
SCRIPTS = REPO / "guest-linux/aarch64/scripts"


class DemoGuestEngineTest(unittest.TestCase):
    def run_zsh(self, source, *args, **environment):
        env = os.environ.copy()
        for key in ("SIM_GUEST_ENGINE", "SIM_GUEST_DISK_IMAGE", "SIM_W5_OE_DISK_IMAGE"):
            env.pop(key, None)
        env.update(environment)
        return subprocess.run(
            ["zsh", "-c", source, "guest-test", *map(str, args)],
            capture_output=True, text=True, env=env, check=False,
        )

    def test_cli_rejects_invalid_os_and_missing_disk_before_launch(self):
        for args, message in (
            (["--guest-engine", "kvm", "--", "run_ub_dual_node_rpc.sh"], "unknown guest engine"),
            (["--guest-engine", "openEuler", "--", "run_ub_dual_node_rpc.sh"], "disk image not found"),
            (["--guest-engine"], "requires a value"),
            (["--", "../run_ub_dual_node_rpc.sh"], "unsupported demo entrypoint"),
        ):
            result = subprocess.run(
                ["zsh", str(SCRIPTS / "run_ub_demo.sh"), *args],
                capture_output=True, text=True, check=False,
                env={"PATH": os.environ["PATH"]},
            )
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn(message, result.stderr)

    def test_cli_preserves_arguments_and_explicit_initramfs_choice(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            shutil.copy(SCRIPTS / "run_ub_demo.sh", root)
            fixture = root / "run_ub_dual_node_rpc.sh"
            fixture.write_text(
                '#!/bin/zsh\nprintf "%s\\n" "$SIM_GUEST_ENGINE" "$SIM_GUEST_DISK_IMAGE" "$@"\n'
            )
            fixture.chmod(0o755)
            result = subprocess.run(
                ["zsh", str(root / "run_ub_demo.sh"), "--guest-engine", "initramfs",
                 "--", fixture.name, "one argument", "$(must-not-run)"],
                capture_output=True, text=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(),
                             ["initramfs", "", "one argument", "$(must-not-run)"])

    def test_initramfs_boot_retains_the_original_arguments(self):
        result = self.run_zsh(
            'source "$1"; ub_guest_boot_prepare /unused /current.cpio.gz run; '
            'ub_guest_boot_args nodeA "console=ttyAMA0 rdinit=/bin/run_app workload=1"; '
            'printf "%s\\n" "${UB_GUEST_BOOT_ARGS[@]}"',
            SCRIPTS / "qemu_ub_common.sh",
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(),
                         ["-initrd", "/current.cpio.gz", "-append",
                          "console=ttyAMA0 rdinit=/bin/run_app workload=1"])

    def test_boot_preparation_failure_cleans_current_resources(self):
        for entrypoint in ("common", "four", "eight"):
            if entrypoint == "common":
                preparation = 'ub_guest_boot_prepare "$ROOT_DIR" "$INITRAMFS_IMAGE" "$RUN_ID"'
            else:
                launcher = SCRIPTS / f"launch_ub_{entrypoint}_node_headless.sh"
                source = launcher.read_text()
                start = source.index('ensure_ub_guest_artifacts "$ROOT_DIR"')
                end_marker = ('\nmkdir -p "$OUT_DIR"' if entrypoint == "four" else
                              '\nif [[ "$SIM_W5_GUEST_BACKEND"')
                end = source.index(end_marker, start)
                preparation = source[start:end]
            with self.subTest(entrypoint=entrypoint), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                (root / "out").mkdir()
                disk = root / "base.qcow2"
                disk.touch()
                image = root / "broken.cpio.gz"
                image.write_text("invalid archive\n")
                stale_cleanup = root / "old-cleanup.sh"
                stale_cleanup.write_text("#!/bin/zsh\nexit 0\n")
                stale_cleanup.chmod(0o755)
                foreign = root / "out/keep-other-run"
                foreign.write_text("owned by another run\n")
                result = self.run_zsh(
                    'set -euo pipefail; source "$1"; '
                    'oe_ensure_lvm2_staging() { return 0; }; '
                    'ensure_ub_guest_artifacts() { return 0; }; '
                    'ROOT_DIR="$2"; INITRAMFS_IMAGE="$3"; KERNEL_IMAGE=/unused; '
                    'RUN_ID=failed-preparation; CLEANUP_SCRIPT="$4"; '
                    'QEMU_IMG_BIN=/bin/true;\n' + preparation,
                    SCRIPTS / "qemu_ub_common.sh", root, image, stale_cleanup,
                    SIM_GUEST_ENGINE="openEuler", SIM_GUEST_DISK_IMAGE=str(disk),
                )
                self.assertNotEqual(result.returncode, 0, result.stderr)
                self.assertIn("not in gzip format", result.stderr)
                self.assertEqual(list((root / "out").glob("guest_boot.*")), [])
                self.assertEqual(foreign.read_text(), "owned by another run\n")

    def test_modular_rootfs_dependencies_share_one_modpost(self):
        source = (SCRIPTS / "build_guest_artifacts.sh").read_text()
        start = source.index("build_native_required_modules() {")
        end = source.index("\ninstall_kernel_uapi_headers()", start)
        function = source[start:end]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / ".config").write_text(
                "CONFIG_NETFS_SUPPORT=m\nCONFIG_FSCACHE=m\nCONFIG_NET_9P=m\n"
                "CONFIG_NET_9P_VIRTIO=y\nCONFIG_9P_FS=m\n"
            )
            result = self.run_zsh(
                function + '\nbuild_native_module_if_enabled() { return 0; }; '
                'run_gnu_make() { printf "%s\\n" "$@"; }; '
                'KERNEL_BUILD_DIR="$1"; KERNEL_SRC_DIR=/kernel; KERNEL_ARCH=arm64; '
                'KERNEL_JOBS=2; build_native_required_modules ""', root,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = result.stdout.splitlines()
            self.assertEqual(lines.count("-C"), 1)
            self.assertEqual(lines[-4:], ["fs/netfs/netfs.ko", "fs/fscache/fscache.ko",
                                         "net/9p/9pnet.ko", "fs/9p/9p.ko"])
            self.assertNotIn("net/9p/9pnet_virtio.ko", lines)

    @unittest.skipUnless(shutil.which("qemu-img") and shutil.which("cpio"),
                         "requires host disk/archive tools, not QEMU guests")
    def test_open_euler_deploys_current_apps_and_owns_fresh_node_disks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "out").mkdir()
            (root / "initramfs").mkdir()
            shutil.copy(REPO / "guest-linux/aarch64/initramfs/init_switch_root",
                        root / "initramfs")
            base = root / "base"
            (base / "bin").mkdir(parents=True)
            for name in ("busybox", "linqu_init", "run_app", "linqu_fixture"):
                binary = base / "bin" / name
                binary.write_text("current-" + name)
                binary.chmod(0o755)
            image = root / "base.cpio.gz"
            pack = self.run_zsh('cd "$1"; find . -print | cpio -o -H newc --quiet | gzip > "$2"',
                                base, image)
            self.assertEqual(pack.returncode, 0, pack.stderr)
            disk = root / "base.qcow2"
            subprocess.run(["qemu-img", "create", "-f", "qcow2", str(disk), "1M"],
                           check=True, capture_output=True)
            before = hashlib.sha256(disk.read_bytes()).hexdigest()
            result = self.run_zsh(
                'set -e; source "$1"; oe_ensure_lvm2_staging() { return 0; }; '
                'ub_guest_boot_prepare "$2" "$3" test; '
                'echo "$UB_GUEST_BOOT_DIR"; '
                'ub_guest_boot_args nodeA "console=ttyAMA0 rdinit=/bin/run_app workload=1"; '
                'printf "%s\\n" "${UB_GUEST_BOOT_ARGS[@]}"; '
                'ub_guest_boot_args nodeB "console=ttyAMA0 rdinit=/bin/run_app workload=2"; '
                'printf "%s\\n" "${UB_GUEST_BOOT_ARGS[@]}"; '
                'ub_guest_boot_append_cleanup "$2/cleanup.sh"',
                SCRIPTS / "qemu_ub_common.sh", root, image,
                SIM_GUEST_ENGINE="openEuler", SIM_GUEST_DISK_IMAGE=str(disk),
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            lines = result.stdout.splitlines()
            boot_dir = Path(lines[0])
            command_line = lines[6]
            self.assertNotIn("rdinit=", command_line)
            self.assertIn("workload=1", command_line)
            self.assertIn("init=/init enforcing=0", command_line)
            overlays = list((boot_dir / "disks").glob("*.qcow2"))
            self.assertEqual(len(overlays), 2)
            for overlay in overlays:
                info = json.loads(subprocess.check_output(
                    ["qemu-img", "info", "--output=json", str(overlay)], text=True))
                self.assertEqual(info["full-backing-filename"], str(disk))
            unpack = root / "unpack"
            unpack.mkdir()
            extracted = self.run_zsh('cd "$1"; gzip -dc "$2" | cpio -id --quiet',
                                     unpack, boot_dir / "boot.cpio.gz")
            self.assertEqual(extracted.returncode, 0, extracted.stderr)
            deployed = unpack / "ub_root_overlay"
            self.assertEqual((deployed / "bin/linqu_fixture").read_text(), "current-linqu_fixture")
            unit = (deployed / "etc/systemd/system/linqu-w5-guest.service").read_text()
            self.assertIn("StandardInput=tty", unit)
            self.assertIn("ExecStart=/bin/busybox sh /bin/run_app", unit)
            self.assertEqual(hashlib.sha256(disk.read_bytes()).hexdigest(), before)
            cleanup = subprocess.run(["zsh", str(root / "cleanup.sh")], capture_output=True)
            self.assertEqual(cleanup.returncode, 0)
            self.assertFalse(boot_dir.exists())


if __name__ == "__main__":
    unittest.main()
