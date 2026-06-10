#!/usr/bin/env python3
import re
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]


class UbDeviceBusyCompletionTest(unittest.TestCase):
    def assert_busy_branch_publishes_completion(self, relpath, device):
        source = (REPO_ROOT / relpath).read_text(encoding="utf-8")
        status = f"{device}_STATUS_COMPLETION_VALID"
        error = f"{device}_ERR_DEVICE_BUSY"

        match = re.search(
            rf"if \(\!\(s->status & {status}\)\) \{{(?P<body>.*?)\n\s*\}}\n"
            rf"\s*s->status \|= {device}_STATUS_ERROR;"
            rf"\n\s*s->error_reg = \(uint32_t\)\(-{error}\);",
            source,
            re.S,
        )
        self.assertIsNotNone(match, f"{device} BUSY branch shape changed")
        body = match.group("body")
        self.assertIn(f"s->cpl.status = {error};", body)
        self.assertIn(f"s->status |= {status};", body)

    def test_npu_busy_doorbell_returns_valid_completion(self):
        self.assert_busy_branch_publishes_completion(
            "vendor/qemu_8.2.0_ub/hw/ub/ub_npu.c",
            "NPU",
        )

    def test_ssd_busy_doorbell_returns_valid_completion(self):
        self.assert_busy_branch_publishes_completion(
            "vendor/qemu_8.2.0_ub/hw/ub/ub_ssd.c",
            "SSD",
        )


if __name__ == "__main__":
    unittest.main()
