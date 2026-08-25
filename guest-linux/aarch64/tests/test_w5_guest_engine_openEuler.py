#!/usr/bin/env python3
"""Contract tests for the openEuler W5 guest engine."""

import pathlib
import subprocess
import unittest

REPO = pathlib.Path(__file__).resolve().parents[3]
AARCH64 = REPO / "guest-linux" / "aarch64"
SCRIPTS = AARCH64 / "scripts"

GUEST_RUNNER = SCRIPTS / "run_llm_infer_eight_node_guest.sh"
W5_RUNTIME = SCRIPTS / "run_w5_inference_cluster_runtime.sh"
LAUNCHER = SCRIPTS / "launch_ub_eight_node_headless.sh"
OE_WRAPPER = SCRIPTS / "run_w5_cluster_qwen3_0_6b_2step_openEuler.sh"
DEEPSEEK_CONFIG = REPO / "w5.deepseek-v4-flash-simpler-openeuler.env"
SWITCH_ROOT = AARCH64 / "initramfs" / "init_switch_root"
COMMON = SCRIPTS / "qemu_ub_common.sh"
OE_SUPER = SCRIPTS / "run-openEuler-simulated-super-node.sh"
CONTAINER_DEPS = SCRIPTS / "prepare_w5_container_deps.sh"
GUEST_BUILDER = SCRIPTS / "build_guest_artifacts.sh"


class W5GuestEngineOpenEulerTest(unittest.TestCase):
    def test_guest_runner_validates_engine_and_disk(self):
        source = GUEST_RUNNER.read_text(encoding="utf-8")
        self.assertIn('SIM_W5_GUEST_ENGINE="${SIM_W5_GUEST_ENGINE:-busybox}"', source)
        self.assertIn("SIM_W5_GUEST_ENGINE must be busybox or openEuler", source)
        self.assertIn("openEuler guest engine requires SIM_W5_OE_DISK_IMAGE", source)
        self.assertIn("build_w4_openEuler_initramfs", source)

    def test_guest_runner_builds_root_overlay_with_unit(self):
        source = GUEST_RUNNER.read_text(encoding="utf-8")
        self.assertIn("ub_root_overlay", source)
        self.assertIn("/bin/linqu_*", source)
        self.assertIn("write_w5_openEuler_systemd_unit", source)
        self.assertIn("ExecStart=/bin/busybox sh /bin/run_app", source)
        self.assertIn("WantedBy=multi-user.target", source)

    def test_switch_root_deploys_overlay_resolving_usrmerge(self):
        source = SWITCH_ROOT.read_text(encoding="utf-8")
        self.assertIn("/ub_root_overlay", source)
        # busybox cp -a silently skips dirs whose dest is a symlink to a dir
        # (usrmerge /bin -> usr/bin); deploy must resolve destinations first.
        self.assertIn("readlink", source)

    def test_switch_root_supports_direct_ext4_and_lvm_images(self):
        source = SWITCH_ROOT.read_text(encoding="utf-8")
        self.assertIn("direct rootfs mounted", source)
        self.assertIn("/dev/vda /dev/vdb", source)
        self.assertIn("direct rootfs not found; activating LVM", source)
        self.assertIn("/sbin/vgchange -ay", source)

    def test_launcher_boots_disk_with_permissive_selinux(self):
        source = LAUNCHER.read_text(encoding="utf-8")
        self.assertIn("SIM_W5_OE_DISK_IMAGE", source)
        self.assertIn("oe_overlays", source)
        self.assertIn("qemu-img create -f qcow2 -b", source)
        self.assertIn("init=/init enforcing=0", source)

    def test_common_shares_oe_helpers(self):
        source = COMMON.read_text(encoding="utf-8")
        self.assertIn("oe_privileged()", source)
        self.assertIn('if [[ "$(id -u)" == "0" ]]', source)
        self.assertIn("oe_privileged losetup", source)
        self.assertIn('oe_privileged mount "$loop_dev"', source)
        self.assertIn('touch "$staging_dir/direct-root"', source)
        self.assertIn("oe_ensure_lvm2_staging()", source)
        self.assertIn("oe_build_boot_skeleton()", source)

    def test_container_deps_cover_openeuler_disk_tools(self):
        source = CONTAINER_DEPS.read_text(encoding="utf-8")
        for package in ("lvm2", "parted", "qemu-img", "qemu-utils"):
            self.assertIn(package, source)
        for tool in ("qemu-img", "partprobe", "vgscan", "vgchange"):
            self.assertIn(tool, source)

    def test_openEuler_root_storage_is_built_into_guest_kernel(self):
        source = GUEST_BUILDER.read_text(encoding="utf-8")
        self.assertIn('KERNEL_BUILD_POLICY_REV="3"', source)
        self.assertIn("build_policy=%s", source)
        self.assertIn("-e VIRTIO \\", source)
        self.assertIn("-e VIRTIO_BLK \\", source)
        self.assertIn("-e VIRTIO_MMIO \\", source)
        self.assertIn("-e VIRTIO_PCI \\", source)
        self.assertIn("-e EXT4_FS \\", source)
        self.assertIn("-e BLK_DEV_DM \\", source)

    def test_oe_wrapper_selects_engine(self):
        source = OE_WRAPPER.read_text(encoding="utf-8")
        self.assertIn("SIM_W5_GUEST_ENGINE=openEuler", source)
        self.assertIn("run_w5_cluster_qwen3_0_6b_2step.sh", source)

    def test_deepseek_config_selects_openeuler_and_live_memory_service(self):
        source = DEEPSEEK_CONFIG.read_text(encoding="utf-8")
        self.assertIn("SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode", source)
        self.assertIn(
            "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash-simpler",
            source,
        )
        self.assertIn("SIM_W5_GUEST_ENGINE=openEuler", source)
        self.assertIn("SIM_W5_OE_DISK_IMAGE=", source)
        self.assertIn("SIM_LLM_INFER_PROMPT_TOKEN_IDS=42", source)
        self.assertIn("SIM_W5_MEMORY_SERVICE=lingqu_memory_service", source)
        self.assertNotIn("SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE", source)
        self.assertIn("Guest OBMM/GSVA references are run-scoped", source)

    def test_deepseek_memory_client_uses_deepseek_model_source(self):
        source = W5_RUNTIME.read_text(encoding="utf-8")
        self.assertIn('model_source_path="$SIM_DEEPSEEK_V4_FLASH"', source)
        self.assertIn('--weights-path "$model_source_path"', source)
        self.assertNotIn(
            "W5 sim-cli orchestration path requires SIM_QWEN3_DENSE_WEIGHTS_PATH",
            source,
        )

    def test_oe_super_node_script_defaults_busybox(self):
        source = OE_SUPER.read_text(encoding="utf-8")
        self.assertIn('BUSYBOX="${BUSYBOX:-$ROOT_DIR/busybox-aarch64}"', source)

    def test_engine_rejects_unknown_value(self):
        result = subprocess.run(
            [
                "zsh",
                str(GUEST_RUNNER),
            ],
            env={
                "PATH": "/usr/bin:/bin",
                "SIM_W5_CLUSTER_NODE_COUNT": "4",
                "SIM_W5_GUEST_ENGINE": "kvm",
            },
            capture_output=True,
            text=True,
            check=False,
            cwd=str(AARCH64),
        )
        self.assertEqual(2, result.returncode)
        self.assertIn("SIM_W5_GUEST_ENGINE must be busybox or openEuler: kvm", result.stderr)


if __name__ == "__main__":
    unittest.main()
