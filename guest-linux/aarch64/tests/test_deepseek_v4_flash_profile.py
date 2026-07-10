#!/usr/bin/env python3
"""
DeepSeek V4 Flash geometry contract tests (stage 1).

These tests validate that the Flash client-side helper exposes the geometry
expected by the plan: 43 layers split over the active pipeline topology,
compressed-attention dimensions, and an OBMM range-flow request builder. mem_service is
infrastructure and must not keep a global active model selector.

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
EIGHT_NODE_RUNNER = ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh"


class DeepseekV4FlashProfileTest(unittest.TestCase):
    def setUp(self):
        self.header = FLASH_H.read_text()
        self.source = FLASH_C.read_text()
        self.profile_header = PROFILE_H.read_text()
        self.profile_source = PROFILE_C.read_text()

    def test_flash_adapter_files_exist(self):
        self.assertTrue(FLASH_H.exists())
        self.assertTrue(FLASH_C.exists())

    def test_flash_range_flow_request_helper_is_declared_and_defined(self):
        self.assertIn(
            "mem_service_deepseek_v4_flash_init_obmm_range_flow_request", self.header
        )
        self.assertIn(
            "mem_service_deepseek_v4_flash_init_obmm_range_flow_request", self.source
        )

    def test_mem_service_profile_has_no_flash_registry(self):
        # mem_service_profile.c is the neutral request initializer; it must not
        # include or register model-specific helpers.
        self.assertNotIn('#include "mem_service_deepseek_v4_flash.h"', self.profile_source)
        self.assertNotIn("mem_service_deepseek_v4_flash_profile", self.profile_source)
        self.assertNotIn("mem_service_lookup_model_profile", self.profile_header)
        self.assertNotIn("mem_service_active_model_profile", self.profile_header)

    def test_flash_geometry_constants_match_ds4_reference(self):
        # Mirror ds4 DS4_SHAPE_FLASH (ds4.c:177-212).
        self.assertIn("#define DEEPSEEK_V4_FLASH_TOTAL_LAYERS 43U", self.source)
        self.assertNotIn("DEEPSEEK_V4_FLASH_PIPELINE_NODES", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_HIDDEN_SIZE 4096ULL", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_KV_HEADS 1ULL", self.source)
        self.assertIn("#define DEEPSEEK_V4_FLASH_HEAD_DIM 512ULL", self.source)
        self.assertIn('#define DEEPSEEK_V4_FLASH_MODEL_KEY "deepseek-v4-flash"', self.source)

    def test_flash_model_key_is_client_supplied(self):
        self.assertIn("mem_service_deepseek_v4_flash_model_key", self.header)
        self.assertIn("mem_service_deepseek_v4_flash_model_key()", self.source)
        self.assertIn("mem_service_init_obmm_range_flow_request", self.source)

    def test_flash_layer_range_uses_active_topology(self):
        self.assertIn("base = layer_count / cluster_node_count", self.source)
        self.assertIn("rem = layer_count % cluster_node_count", self.source)
        self.assertIn("cluster_node_count == 0", self.source)
        self.assertIn(
            "cluster_node_count > DEEPSEEK_V4_FLASH_TOTAL_LAYERS", self.source
        )
        self.assertIn("cluster_node_count,", self.source)

    def test_flash_handoff_uses_flash_hidden_size(self):
        # step0 = full prefill range (hidden_size * prefill_tokens * 2);
        # step>0 = decode range (hidden_size * decode_tokens * 2).
        self.assertIn("DEEPSEEK_V4_FLASH_PREFILL_TOKENS 128ULL", self.source)
        self.assertIn("DEEPSEEK_V4_FLASH_DECODE_TOKENS 1ULL", self.source)
        self.assertIn("mem_service_deepseek_v4_flash_decode_hidden_bytes()", self.source)

    def test_flash_request_uses_neutral_mem_service_contract(self):
        self.assertIn("struct mem_service_obmm_range_flow_request", self.header)
        self.assertIn("mem_service_init_obmm_range_flow_request", self.source)
        self.assertIn("NULL);", self.source)

    def test_flash_does_not_reuse_qwen3_placement_service(self):
        self.assertNotIn("mem_service_publish_qwen3_layer_range_placements", self.source)
        self.assertNotIn("flash_publish_layer_range_placements", self.source)
        self.assertIn("mem_service_deepseek_v4_flash_layer_range_for_node", self.source)

    def test_eight_node_harness_validates_flash_range_and_first_token(self):
        runner = EIGHT_NODE_RUNNER.read_text()

        self.assertIn("is_deepseek_v4_flash_profile", runner)
        self.assertIn("is_model_range_profile", runner)
        self.assertIn("deepseek_v4_flash_runtime_input_loaded", runner)
        self.assertIn("transport=gsva materialize=local_copy", runner)
        self.assertIn("deepseek_v4_flash_first_token", runner)
        self.assertIn("tokens/deepseek-v4-flash", runner)
        self.assertIn('"flash_weight_catalog"', runner)

    def test_flash_artifact_gate_does_not_require_qwen_stores(self):
        runner = EIGHT_NODE_RUNNER.read_text()
        function_start = runner.index("validate_w5_artifact_sizes()")
        function_end = runner.index("emit_w5_inference_run_report()", function_start)
        function_body = runner[function_start:function_end]
        flash_branch = function_body.index(
            'is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"'
        )
        qwen_store_gate = function_body.index('"memory_store_json"')

        self.assertLess(flash_branch, qwen_store_gate)
        flash_artifact_gate = function_body[flash_branch:qwen_store_gate]
        self.assertIn('"flash_weight_catalog"', flash_artifact_gate)
        self.assertIn("67108864", flash_artifact_gate)
        self.assertIn("1 || return 1\n    return 0", flash_artifact_gate)


if __name__ == "__main__":
    unittest.main()
