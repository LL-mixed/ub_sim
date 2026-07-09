#!/usr/bin/env python3
"""
DeepSeek V4 Flash expert route flow and expert cache contract tests (stage 2).

Validates the C-side stage-2 MoE helpers: weight-tile addressing, route-decision
record keys, and the node-side LRU expert cache simulator (mirrors ds4_ssd.c).
These read source directly (no QEMU/guest run), matching the record-recycling
test pattern. The cross-layer handoff interface is intentionally untouched.
"""

import shutil
import subprocess
import tempfile
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
        self.assertIn("mem_service_expert_weight_tile_ref_init", self.route_h)
        self.assertIn("mem_service_expert_weight_tile_ref_from_catalog_file", self.route_h)
        self.assertIn("struct mem_service_expert_weight_tile_ref", self.route_h)
        self.assertIn("payload_checksum", self.route_h)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_IQ2_XXS", self.route_h)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_Q2_K", self.route_h)
        self.assertIn("MEM_SERVICE_EXPERT_MAX_EXPERTS 256U", self.route_h)
        self.assertIn("MEM_SERVICE_EXPERT_WEIGHT_TILE_DEFAULT_BYTES", self.route_h)

    def test_route_decision_record_key(self):
        self.assertIn('route/%s/step%" PRIu64 "/layer%u/token%u', self.route_c)
        self.assertIn("mem_service_expert_route_record_key", self.route_h)
        self.assertIn("mem_service_expert_route_decision_for_decode", self.route_h)

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
        self.assertIn("uint64_t compute_time_us;", self.cache_h)
        self.assertIn("uint64_t miss_load_time_us;", self.cache_h)
        self.assertIn("uint64_t estimated_latency_us;", self.cache_h)

    def test_cache_latency_model_is_explicit(self):
        self.assertIn("MEM_SERVICE_EXPERT_CACHE_COMPUTE_US_PER_TOUCH", self.cache_h)
        self.assertIn("MEM_SERVICE_EXPERT_CACHE_LOAD_BYTES_PER_US", self.cache_h)
        self.assertIn("expert_cache_fill_latency", self.cache_c)
        self.assertIn("estimated_latency_us", self.cache_c)

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
        self.assertIn("mem_service_expert_route_decision_for_decode", self.source)
        self.assertIn("mem_service_expert_route_record_key", self.source)
        self.assertIn("mem_service_expert_weight_tile_ref_init", self.source)
        self.assertIn("mem_service_expert_weight_tile_ref_from_catalog_file", self.source)
        self.assertIn("mem_service_expert_cache_touch", self.source)
        self.assertIn("MEM_SERVICE_EXPERT_QUANT_IQ2_XXS", self.source)
        self.assertIn("MEM_SERVICE_EXPERT_WEIGHT_TILE_DEFAULT_BYTES", self.source)
        self.assertIn("SIM_W5_FLASH_WEIGHT_CATALOG", self.source)
        self.assertIn('"weight_catalog"', self.source)
        self.assertIn("source=%s status=object_ref_ready", self.source)
        self.assertIn("status=object_ref_ready", self.source)
        self.assertIn("moe expert weight catalog resolve failed", self.source)
        self.assertIn("payload_checksum", self.source)
        self.assertIn("cache_hit=%u", self.source)

    def test_dispatch_called_per_owned_layer_before_compute(self):
        # The decode round iterates owned layers and dispatches MoE forward.
        self.assertIn("w4_layer_forward_dispatch(lid, guest_decode_step", self.source)
        self.assertIn("fail moe layer forward dispatch failed", self.source)
        self.assertIn("moe_expert_cache_summary", self.source)
        self.assertIn("estimated_latency_us", self.source)

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


class ExpertRouteAndCacheBehaviorTest(unittest.TestCase):
    def test_c_helpers_compile_and_run_behavior_checks(self):
        cc = shutil.which("cc") or shutil.which("clang") or shutil.which("gcc")
        if not cc:
            self.skipTest("no C compiler available")

        helper = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mem_service_expert_cache.h"
#include "mem_service_expert_route_flow.h"

static int fail(const char *message)
{
    fprintf(stderr, "%s\n", message);
    return 1;
}

int main(int argc, char **argv)
{
    struct mem_service_expert_route_decision decision;
    struct mem_service_expert_weight_tile_ref tile_ref;
    struct mem_service_expert_cache cache;
    struct mem_service_expert_cache_stats stats;
    uint32_t experts[6] = { 9, 3, 5, 1, 7, 2 };
    char route_key[128];
    uint32_t i;

    if (argc != 2) {
        return fail("usage: expert_route_cache_check <catalog>");
    }
    if (mem_service_expert_route_decision_for_decode(&decision, 3, 7, 0, 256) != 0) {
        return fail("decode route decision failed");
    }
    if (decision.step_index != 3 || decision.layer_id != 7 ||
        decision.token_index != 0 ||
        decision.active_expert_count != MEM_SERVICE_EXPERT_ROUTE_TOP_K) {
        return fail("decode route decision metadata mismatch");
    }
    for (i = 1; i < decision.active_expert_count; ++i) {
        if (decision.active_experts[i - 1] >= decision.active_experts[i]) {
            return fail("decode route experts must be sorted unique");
        }
    }
    if (mem_service_expert_route_record_key(route_key,
                                            sizeof(route_key),
                                            "deepseek-v4-flash",
                                            decision.step_index,
                                            decision.layer_id,
                                            decision.token_index) != 0) {
        return fail("route key build failed");
    }
    if (strcmp(route_key, "route/deepseek-v4-flash/step3/layer7/token0") != 0) {
        return fail("route key mismatch");
    }
    if (mem_service_expert_route_decision_init(&decision, 4, 8, 0, experts, 6) != 0) {
        return fail("manual route decision init failed");
    }
    if (decision.active_experts[0] != 1 || decision.active_experts[1] != 2 ||
        decision.active_experts[2] != 3 || decision.active_experts[3] != 5 ||
        decision.active_experts[4] != 7 || decision.active_experts[5] != 9) {
        return fail("manual route decision must sort experts");
    }
    experts[5] = 9;
    if (mem_service_expert_route_decision_init(&decision, 4, 8, 0, experts, 6) == 0) {
        return fail("duplicate expert must fail closed");
    }
    if (mem_service_expert_weight_tile_ref_init(&tile_ref,
                                                "deepseek-v4-flash",
                                                7,
                                                42,
                                                MEM_SERVICE_EXPERT_QUANT_IQ2_XXS,
                                                2048ULL * 1024ULL) != 0) {
        return fail("weight tile ref init failed");
    }
    if (strcmp(tile_ref.object_key,
               "weights/deepseek-v4-flash/layer7/expert42/iq2_xxs") != 0 ||
        tile_ref.payload_bytes != 2048ULL * 1024ULL ||
        tile_ref.payload_checksum != 0x22b4d5a1fd527586ULL) {
        return fail("weight tile ref mismatch");
    }
    if (mem_service_expert_weight_tile_ref_from_catalog_file(&tile_ref,
                                                             argv[1],
                                                             "deepseek-v4-flash",
                                                             7,
                                                             42) != 0) {
        return fail("weight tile catalog resolve failed");
    }
    if (strcmp(tile_ref.object_key,
               "weights/deepseek-v4-flash/layer7/expert42/q2_k") != 0 ||
        strcmp(tile_ref.quant, MEM_SERVICE_EXPERT_QUANT_Q2_K) != 0 ||
        tile_ref.payload_bytes != 4096 ||
        tile_ref.payload_checksum != 0x12345678ULL) {
        return fail("weight tile catalog ref mismatch");
    }

    mem_service_expert_cache_init(&cache, 2, 128);
    if (mem_service_expert_cache_touch(&cache, 1, 1)) {
        return fail("first touch must miss");
    }
    if (!mem_service_expert_cache_touch(&cache, 1, 1)) {
        return fail("second touch must hit");
    }
    (void)mem_service_expert_cache_touch(&cache, 1, 2);
    (void)mem_service_expert_cache_touch(&cache, 1, 3);
    mem_service_expert_cache_stats(&cache, &stats);
    if (stats.hits != 1 || stats.misses != 3 ||
        stats.evictions != 1 || stats.pread_bytes != 384) {
        return fail("cache stats mismatch");
    }
    if (stats.compute_us_per_touch != 2 ||
        stats.load_bytes_per_us != 4096 ||
        stats.compute_time_us != 8 ||
        stats.miss_load_time_us != 1 ||
        stats.estimated_latency_us != 8) {
        return fail("cache latency estimate mismatch");
    }
    puts("ok");
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmpdir:
            tmp = Path(tmpdir)
            helper_c = tmp / "expert_route_cache_check.c"
            helper_bin = tmp / "expert_route_cache_check"
            catalog = tmp / "weight.catalog"
            helper_c.write_text(helper, encoding="utf-8")
            catalog.write_text(
                "\n".join(
                    [
                        "source_kind=fixture model_key=deepseek-v4-flash total_layers=43 experts_per_layer=256 checksum_algorithm=deterministic-v1",
                        "tile layer=7 expert=42 quant=q2_k payload_bytes=4096 payload_checksum=0x12345678",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            compile_result = subprocess.run(
                [
                    cc,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    f"-I{SERVICE_DIR}",
                    str(helper_c),
                    str(ROUTE_C),
                    str(CACHE_C),
                    "-o",
                    str(helper_bin),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                compile_result.returncode,
                0,
                compile_result.stderr + compile_result.stdout,
            )
            run_result = subprocess.run(
                [str(helper_bin), str(catalog)],
                capture_output=True,
                text=True,
                check=False,
            )
        self.assertEqual(run_result.returncode, 0, run_result.stderr)
        self.assertEqual(run_result.stdout, "ok\n")


if __name__ == "__main__":
    unittest.main()
