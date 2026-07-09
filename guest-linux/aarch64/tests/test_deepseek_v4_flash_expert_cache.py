#!/usr/bin/env python3
"""
DeepSeek V4 Flash expert route flow and expert cache contract tests (stage 2).

Validates the C-side stage-2 MoE helpers: weight-tile addressing, route-decision
record keys, and the node-side LRU expert cache simulator (mirrors ds4_ssd.c).
These read source directly (no QEMU/guest run), matching the record-recycling
test pattern. The cross-layer handoff interface is intentionally untouched.
"""

import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
ROUTE_H = SERVICE_DIR / "mem_service_expert_route_flow.h"
ROUTE_C = SERVICE_DIR / "mem_service_expert_route_flow.c"
CACHE_H = SERVICE_DIR / "mem_service_expert_cache.h"
CACHE_C = SERVICE_DIR / "mem_service_expert_cache.c"
LLM_INFER_C = ROOT / "apps" / "llm_infer" / "llm_infer.c"


class ExpertRouteFlowTest(unittest.TestCase):
    def setUp(self):
        self.route_h = ROUTE_H.read_text()
        self.route_c = ROUTE_C.read_text()

    def test_files_exist(self):
        self.assertTrue(ROUTE_H.exists())
        self.assertTrue(ROUTE_C.exists())

    def test_weight_tile_key_addressing(self):
        # Addressing is (model, layer, expert_id, quant) per plan section 3.3.
        self.assertIn("weights/%s/layer%u/expert%u/%s", self.route_c)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_IQ2_XXS", self.route_h)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_Q2_K", self.route_h)

    def test_route_decision_record_key(self):
        self.assertIn("route/%s/layer%u/token%u", self.route_c)
        self.assertIn("mem_service_expert_route_record_key", self.route_h)

    def test_route_top_k_matches_flash(self):
        self.assertIn("#define MEM_SERVICE_EXPERT_ROUTE_TOP_K 6U", self.route_h)


class ExpertCacheTest(unittest.TestCase):
    def setUp(self):
        self.cache_h = CACHE_H.read_text()
        self.cache_c = CACHE_C.read_text()

    def test_files_exist(self):
        self.assertTrue(CACHE_H.exists())
        self.assertTrue(CACHE_C.exists())

    def test_cache_stats_fields(self):
        self.assertIn("uint64_t hits;", self.cache_h)
        self.assertIn("uint64_t misses;", self.cache_h)
        self.assertIn("uint64_t evictions;", self.cache_h)
        self.assertIn("uint64_t pread_bytes;", self.cache_h)

    def test_cache_lru_semantics_in_source(self):
        # touch() must promote on hit and evict the front (LRU) on capacity.
        self.assertIn("mem_service_expert_cache_touch", self.cache_h)
        self.assertIn("Hit: promote to MRU", self.cache_c)
        self.assertIn("evict LRU (front)", self.cache_c)

    def test_cache_preload_hotlist(self):
        self.assertIn("mem_service_expert_cache_preload", self.cache_h)
        self.assertIn("no misses counted", self.cache_h)

    def test_cache_does_not_hold_payload(self):
        # Plan section 3.3: cache is residency/stats only, not payload bytes.
        self.assertIn("payload bytes", self.cache_h)
        self.assertIn("only tracks residency", self.cache_h)


class LlmInferMoeDispatchTest(unittest.TestCase):
    """Validate the per-profile MoE forward dispatch in the guest decode app."""

    def setUp(self):
        self.source = LLM_INFER_C.read_text()

    def test_profile_detection_helpers_exist(self):
        self.assertIn("is_deepseek_v4_flash_profile", self.source)
        self.assertIn("is_moe_profile", self.source)
        self.assertIn('strcmp(profile, "deepseek-v4-flash")', self.source)
        self.assertIn('strcmp(profile, "deepseek_v4_flash")', self.source)

    def test_moe_dispatch_records_route_and_fetches_experts(self):
        # Per-layer MoE: record route decision + fetch expert weight tiles.
        self.assertIn("w4_layer_forward_dispatch_moe", self.source)
        self.assertIn("mem_service_expert_route_record_key", self.source)
        self.assertIn("mem_service_expert_weight_tile_key", self.source)
        self.assertIn("mem_service_expert_cache_touch", self.source)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_IQ2_XXS", self.source)

    def test_dispatch_called_per_owned_layer_before_compute(self):
        # The decode round iterates owned layers and dispatches MoE forward.
        self.assertIn("w4_layer_forward_dispatch(lid, guest_decode_step", self.source)
        self.assertIn("moe_expert_cache_summary", self.source)

    def test_layer_range_resolution_is_profile_aware(self):
        # 8-node dispatch resolves model geometry in llm_infer, then uses
        # mem_service only as the object transport/cache infrastructure.
        self.assertIn("w4_runtime_layer_range_for_node", self.source)
        self.assertIn("mem_service_deepseek_v4_flash_layer_range_for_node", self.source)
        self.assertNotIn("mem_service_model_layer_range_for_node", self.source)

    def test_guest_obmm_range_flow_request_is_client_built(self):
        self.assertIn("w4_runtime_init_obmm_range_flow_request", self.source)
        self.assertIn("mem_service_deepseek_v4_flash_init_obmm_range_flow_request", self.source)
        self.assertIn("struct mem_service_obmm_range_flow_request range_request", self.source)
        self.assertIn(
            "mem_service_obmm_service_v0_publish_resolve(&svc,\n"
            "                                                    &range_request,",
            self.source,
        )

    def test_dispatch_guards_on_moe_profile(self):
        # The dense path must not run MoE expert routing.
        self.assertIn("if (is_moe_profile())", self.source)

    def test_expert_cache_headers_are_included(self):
        self.assertIn("mem_service_expert_route_flow.h", self.source)
        self.assertIn("mem_service_expert_cache.h", self.source)
        self.assertIn("mem_service_profile.h", self.source)


if __name__ == "__main__":
    unittest.main()
