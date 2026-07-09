#!/usr/bin/env python3
"""
DeepSeek V4 Flash model adapter geometry contract tests (stage 1).

These tests validate that the Flash adapter registered in the mem_service
profile registry exposes the geometry expected by the plan: 43 layers over 8
pipeline nodes, MoE expert counts, compressed-attention dimensions. They read
the adapter source/header directly (the same pattern as
test_mem_service_record_recycling); no QEMU/guest run is required.

Stage 1 scope is geometry only. Real MoE routing / expert aggregation /
expert cache is stage 2 and is intentionally not asserted here.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
FLASH_H = SERVICE_DIR / "mem_service_deepseek_v4_flash.h"
FLASH_C = SERVICE_DIR / "mem_service_deepseek_v4_flash.c"
PROFILE_H = SERVICE_DIR / "mem_service_profile.h"
PROFILE_C = SERVICE_DIR / "mem_service_profile.c"


class DeepseekV4FlashProfileTest(unittest.TestCase):
    def setUp(self):
        self.header = FLASH_H.read_text()
        self.source = FLASH_C.read_text()
        self.profile_header = PROFILE_H.read_text()
        self.profile_source = PROFILE_C.read_text()

    def test_flash_adapter_files_exist(self):
        self.assertTrue(FLASH_H.exists())
        self.assertTrue(FLASH_C.exists())

    def test_flash_profile_accessor_is_declared_and_defined(self):
        self.assertIn(
            "mem_service_deepseek_v4_flash_profile", self.header
        )
        self.assertIn(
            "mem_service_deepseek_v4_flash_profile(void)", self.source
        )

    def test_flash_profile_is_registered_in_profile_table(self):
        # profile.c must pull in the Flash header and register the accessor.
        self.assertIn('#include "mem_service_deepseek_v4_flash.h"', self.profile_source)
        self.assertIn(
            "mem_service_deepseek_v4_flash_profile()", self.profile_source
        )

    def test_flash_geometry_constants_match_ds4_reference(self):
        # Mirror ds4 DS4_SHAPE_FLASH (ds4.c:177-212).
        self.assertIn("#define DEEPSEEK_V4_FLASH_TOTAL_LAYERS 43U", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_PIPELINE_NODES 8U", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_HIDDEN_SIZE 4096ULL", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_KV_HEADS 1ULL", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_HEAD_DIM 512ULL", self.source)
        self.assertIn('#define DEEPSEEK_V4_FLASH_MODEL_KEY "deepseek-v4-flash"', self.source)

    def test_flash_profile_name_and_namespace(self):
        self.assertIn('.name = "deepseek-v4-flash"', self.source)
        self.assertIn('.key_namespace = "deepseek-v4-flash"', self.source)

    def test_flash_layer_range_balances_43_over_8_nodes(self):
        # The C adapter uses base = 43/8 = 5, rem = 3, so nodes 0-2 get 6
        # layers and nodes 3-7 get 5 layers. Verify the guard requires the
        # Flash pipeline-node count.
        self.assertIn("DEEPSEEK_V4_FLASH_PIPELINE_NODES", self.source)
        # base+rem split logic present (same shape as llm_infer_qwen3).
        self.assertIn("base = layer_count / cluster_node_count", self.source)
        self.assertIn("rem = layer_count % cluster_node_count", self.source)

    def test_flash_handoff_uses_flash_hidden_size(self):
        # step0 = full prefill range (hidden_size * prefill_tokens * 2);
        # step>0 = decode range (hidden_size * decode_tokens * 2).
        self.assertIn("DEEPSEEK_V4_FLASH_PREFILL_TOKENS 128ULL", self.source)
        self.assertIn("DEEPSEEK_V4_FLASH_DECODE_TOKENS 1ULL", self.source)
        self.assertIn("flash_decode_hidden_bytes()", self.source)

    def test_flash_reuses_shared_obmm_kinds_in_stage1(self):
        # Stage 1 shares the qwen3 OBMM layout/kinds; stage 2 will split.
        self.assertIn("MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT", self.source)
        self.assertIn("MEM_SERVICE_OBMM_KIND_QWEN3_KV_STATE", self.source)

    def test_flash_reuses_placement_service(self):
        # Flash reuses the shared placement record mechanism (model-neutral
        # struct) via wrappers that cast the neutral struct name.
        self.assertIn("flash_publish_layer_range_placements", self.source)
        self.assertIn("flash_read_layer_range_placement", self.source)
        self.assertIn("flash_find_layer_range_predecessor", self.source)
        self.assertIn(
            "mem_service_publish_qwen3_layer_range_placements", self.source
        )


if __name__ == "__main__":
    unittest.main()
