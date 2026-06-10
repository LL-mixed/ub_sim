#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


class UbDeviceInstantiationContractTest(unittest.TestCase):
    def _doc_stats(self, relpath):
        doc = (REPO_ROOT / relpath).read_text(encoding="utf-8")
        section = doc.split("V1 stats:", 1)[1]
        match = re.search(r"```text\n(?P<body>.*?)\n```", section, re.S)
        self.assertIsNotNone(match)
        return [
            line.strip()
            for line in match.group("body").splitlines()
            if line.strip()
        ]

    def _qemu_stats(self, relpath, struct_name, prefix):
        source = (REPO_ROOT / relpath).read_text(encoding="utf-8")
        match = re.search(
            rf"typedef struct {struct_name} \{{(?P<body>.*?)\}} {struct_name};",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        fields = re.findall(r"\buint64_t\s+([a-z0-9_]+);", match.group("body"))
        return [f"{prefix}_{field}" for field in fields]

    def _qemu_define(self, relpath, name):
        source = (REPO_ROOT / relpath).read_text(encoding="utf-8")
        match = re.search(rf"#define\s+{name}\s+(0x[0-9a-fA-F]+|\d+)", source)
        self.assertIsNotNone(match)
        return int(match.group(1), 0)

    def test_design_docs_use_machine_instantiation_contract(self):
        npu_doc = (REPO_ROOT / "docs/sim_ub_attached_npu_design.md").read_text(
            encoding="utf-8"
        )
        ssd_doc = (REPO_ROOT / "docs/sim_ub_attached_ssd_design.md").read_text(
            encoding="utf-8"
        )

        self.assertIn("virt` machine instantiates one `ub-npu`", npu_doc)
        self.assertIn("UB_SIM_SKIP_DEVICES=npu", npu_doc)
        self.assertNotIn("-device ub-npu", npu_doc)

        self.assertIn("virt` machine instantiates one `ub-ssd`", ssd_doc)
        self.assertIn("UB_SIM_SKIP_DEVICES=ssd", ssd_doc)
        self.assertNotIn("-device ub-ssd", ssd_doc)

    def test_virt_machine_auto_creates_devices_with_skip_control(self):
        virt = (REPO_ROOT / "vendor/qemu_8.2.0_ub/hw/arm/virt.c").read_text(
            encoding="utf-8"
        )

        self.assertIn('g_getenv("UB_SIM_SKIP_DEVICES")', virt)
        self.assertIn('qdev_new("ub-npu")', virt)
        self.assertIn('qdev_new("ub-ssd")', virt)
        self.assertIn('strstr(skip_devices, "npu")', virt)
        self.assertIn('strstr(skip_devices, "ssd")', virt)

    def test_wait_ioctl_uses_completion_abi(self):
        npu_doc = (REPO_ROOT / "docs/sim_ub_attached_npu_design.md").read_text(
            encoding="utf-8"
        )
        ssd_doc = (REPO_ROOT / "docs/sim_ub_attached_ssd_design.md").read_text(
            encoding="utf-8"
        )
        npu_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_npu.h"
        ).read_text(encoding="utf-8")
        ssd_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_ssd.h"
        ).read_text(encoding="utf-8")

        self.assertIn("ioctl(UB_NPU_WAIT, struct ub_npu_cpl_v1)", npu_doc)
        self.assertNotIn("ub_npu_wait_v1", npu_doc)
        self.assertIn(
            "#define UB_NPU_WAIT\t\t_IOR(UB_NPU_IOC_MAGIC, 2, struct ub_npu_cpl_v1)",
            npu_uapi,
        )

        self.assertIn("ioctl(UB_SSD_WAIT, struct ub_ssd_cpl_v1)", ssd_doc)
        self.assertNotIn("ub_ssd_wait_v1", ssd_doc)
        self.assertIn(
            "#define UB_SSD_WAIT\t\t_IOR(UB_SSD_IOC_MAGIC, 2, struct ub_ssd_cpl_v1)",
            ssd_uapi,
        )

    def test_buffer_descriptors_use_canonical_token_fields(self):
        npu_doc = (REPO_ROOT / "docs/sim_ub_attached_npu_design.md").read_text(
            encoding="utf-8"
        )
        ssd_doc = (REPO_ROOT / "docs/sim_ub_attached_ssd_design.md").read_text(
            encoding="utf-8"
        )
        npu_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_npu.h"
        ).read_text(encoding="utf-8")
        ssd_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_ssd.h"
        ).read_text(encoding="utf-8")

        self.assertNotIn("gsva_token_v1", npu_doc)
        self.assertIn("uint32_t token_id;", npu_doc)
        self.assertIn("uint32_t token_value;", npu_doc)
        self.assertIn("__u32\ttoken_id;", npu_uapi)
        self.assertIn("__u32\ttoken_value;", npu_uapi)

        self.assertNotIn("gsva_token_v1", ssd_doc)
        self.assertIn("uint32_t token_id;", ssd_doc)
        self.assertIn("uint32_t token_value;", ssd_doc)
        self.assertIn("__u32\ttoken_id;", ssd_uapi)
        self.assertIn("__u32\ttoken_value;", ssd_uapi)

    def test_design_docs_list_stable_uapi_opcodes_and_statuses(self):
        npu_doc = (REPO_ROOT / "docs/sim_ub_attached_npu_design.md").read_text(
            encoding="utf-8"
        )
        ssd_doc = (REPO_ROOT / "docs/sim_ub_attached_ssd_design.md").read_text(
            encoding="utf-8"
        )

        for symbol in (
            "NPU_OP_NOOP",
            "NPU_OP_MEMCOPY",
            "NPU_OP_FILL",
            "NPU_OP_VECTOR_ADD_U32",
            "NPU_OP_CHECKSUM64",
            "NPU_CMD_INJECT_COH_TIMEOUT",
            "NPU_ERR_DEVICE_BUSY",
        ):
            self.assertIn(symbol, npu_doc)

        for symbol in (
            "SSD_OP_BLOCK_WRITE",
            "SSD_OP_BLOCK_READ",
            "SSD_OP_BLOCK_SEAL",
            "SSD_OP_BLOCK_TOMBSTONE",
            "SSD_OP_FLUSH",
            "SSD_OP_STAT",
            "SSD_OP_EXPORT_SNAPSHOT",
            "SSD_OP_IMPORT_SNAPSHOT",
            "SSD_CMD_INJECT_COH_TIMEOUT",
            "SSD_ERR_BAD_SNAPSHOT",
        ):
            self.assertIn(symbol, ssd_doc)

    def test_failure_injection_flags_are_in_uapi(self):
        npu_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_npu.h"
        ).read_text(encoding="utf-8")
        ssd_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_ssd.h"
        ).read_text(encoding="utf-8")

        self.assertIn("#define NPU_CMD_INJECT_COH_TIMEOUT", npu_uapi)
        self.assertIn("#define SSD_CMD_INJECT_COH_TIMEOUT", ssd_uapi)

    def test_design_stats_match_qemu_struct_order(self):
        self.assertEqual(
            self._qemu_stats("vendor/qemu_8.2.0_ub/hw/ub/ub_npu.c",
                             "UbNpuStats", "npu"),
            self._doc_stats("docs/sim_ub_attached_npu_design.md"),
        )
        self.assertEqual(
            self._qemu_stats("vendor/qemu_8.2.0_ub/hw/ub/ub_ssd.c",
                             "UbSsdStats", "ssd"),
            self._doc_stats("docs/sim_ub_attached_ssd_design.md"),
        )

    def test_ssd_stats_window_does_not_overlap_backend_profile(self):
        qemu_source = "vendor/qemu_8.2.0_ub/hw/ub/ub_ssd.c"
        stats = self._qemu_stats(qemu_source, "UbSsdStats", "ssd")
        stats_off = self._qemu_define(qemu_source, "SSD_STATS_OFF")
        stats_size = self._qemu_define(qemu_source, "SSD_STATS_SIZE")
        backend_off = self._qemu_define(qemu_source, "SSD_BACKEND_PROFILE_OFF")
        ssd_uapi = (
            REPO_ROOT / "guest-linux/kernel_ub/include/uapi/ub/ub_ssd.h"
        ).read_text(encoding="utf-8")

        uapi_backend = re.search(
            r"#define\s+SSD_BACKEND_PROFILE_OFF\s+(0x[0-9a-fA-F]+|\d+)",
            ssd_uapi,
        )
        self.assertIsNotNone(uapi_backend)
        self.assertEqual(stats_size, len(stats) * 8)
        self.assertLessEqual(stats_off + stats_size, backend_off)
        self.assertEqual(int(uapi_backend.group(1), 0), backend_off)


if __name__ == "__main__":
    unittest.main()
