#!/usr/bin/env python3
"""Contract tests for the reproducible DeepSeek openEuler W5 config."""

import pathlib
import unittest


REPO = pathlib.Path(__file__).resolve().parents[3]
CONFIG = REPO / "w5.deepseek-v4-flash-simpler-openeuler.env"


class W5DeepSeekOpenEulerConfigTest(unittest.TestCase):
    def test_config_uses_live_memory_service_without_stale_post_run_refs(self):
        source = CONFIG.read_text(encoding="utf-8")

        self.assertIn("SIM_UAPI_W5_PROFILE=deepseek_v4_flash_decode", source)
        self.assertIn(
            "SIM_UAPI_W4_CHIPBACKEND_PROFILE=deepseek-v4-flash-simpler",
            source,
        )
        self.assertIn("SIM_W5_MEMORY_SERVICE=lingqu_memory_service", source)
        self.assertIn("SIM_W5_GUEST_ENGINE=openEuler", source)
        self.assertIn("Guest OBMM/GSVA references are run-scoped", source)
        self.assertNotIn("SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE", source)


if __name__ == "__main__":
    unittest.main()
