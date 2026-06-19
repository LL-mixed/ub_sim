import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
SERVICE_C = SERVICE_DIR / "mem_service.c"
SERVICE_H = SERVICE_DIR / "mem_service.h"
GUEST_C = ROOT / "apps" / "llm_infer" / "llm_infer.c"
BUILD_INITRAMFS = ROOT / "scripts" / "build_initramfs.sh"
RUN_APP = ROOT / "initramfs" / "run_app"
COMPONENTS_README = ROOT / "components" / "README.md"
CLI_DIR = ROOT / "apps" / "mem_service"
FOUR_NODE_W4_RUNNER = ROOT / "scripts" / "run_ub_four_node_w4_guest.sh"
EIGHT_NODE_W4_RUNNER = ROOT / "scripts" / "run_ub_eight_node_w4_guest.sh"
SIM_UAPI_RS = REPO_ROOT / "crates" / "sim-uapi" / "src" / "lib.rs"


class MemServiceRecordRecyclingTests(unittest.TestCase):
    def test_mem_service_has_cli_without_demo_naming(self):
        build_script = BUILD_INITRAMFS.read_text()
        run_app = RUN_APP.read_text()
        components_readme = COMPONENTS_README.read_text()

        self.assertIn("Components do not install guest binaries directly", components_readme)
        self.assertIn(
            'MEM_SERVICE_SRC="$ROOT_DIR/components/mem_service/mem_service.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLI_SRC="$ROOT_DIR/apps/mem_service/mem_service.c"',
            build_script,
        )
        self.assertIn('MEM_SERVICE_CLI_BIN="$OUT_DIR/linqu_mem_service"', build_script)
        self.assertIn(
            '"$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"',
            build_script,
        )
        self.assertIn("linqu_mem_service", build_script)
        self.assertIn("linqu_mem_service", run_app)
        self.assertIn("linqu_mem_service=1", run_app)
        self.assertIn("run_binary \"linqu_mem_service\" /bin/linqu_mem_service --smoke", run_app)
        self.assertTrue((CLI_DIR / "mem_service.c").exists())
        self.assertTrue((CLI_DIR / "Makefile").exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.c").exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.h").exists())
        self.assertFalse((ROOT / "apps" / "mem_service_demo").exists())

    def test_record_caps_support_long_decode_runs(self):
        header = SERVICE_H.read_text()
        source = SERVICE_C.read_text()

        max_records = re.search(r"#define MEM_SERVICE_MAX_RECORDS\s+(\d+)U", header)
        cluster_records = re.search(r"#define MEM_SERVICE_CLUSTER_MAX_RECORDS\s+(\d+)", source)

        self.assertIsNotNone(max_records)
        self.assertIsNotNone(cluster_records)
        self.assertGreaterEqual(int(max_records.group(1)), 1024)
        self.assertGreaterEqual(int(cluster_records.group(1)), 1024)

    def test_full_record_table_recycles_old_qwen3_runtime_records(self):
        source = SERVICE_C.read_text()

        self.assertIn("MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS", source)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", source)
        self.assertIn('strstr(key, "decode-step")', source)
        self.assertIn('strstr(key, "/step/")', source)
        self.assertIn("rec = mem_service_alloc_record(svc);", source)
        self.assertIn("rec = mem_service_recycle_qwen3_runtime_record(svc, key);", source)

    def test_qwen3_kv_state_uses_tiered_block_spans(self):
        source = SERVICE_C.read_text()

        tier_names = [
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES",
        ]
        tier_values = []

        for tier in tier_names:
            self.assertIn(tier, source)
            match = re.search(rf"#define {tier}\s+0x([0-9a-fA-F]+)ULL", source)
            if match:
                tier_values.append(int(match.group(1), 16))

        slot_bytes = re.search(
            r"#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES\s+0x([0-9a-fA-F]+)ULL",
            source,
        )
        self.assertIsNotNone(slot_bytes)
        tier_values.append(int(slot_bytes.group(1), 16))

        max_block_bytes = max(tier_values)
        over_max_payload_bytes = max_block_bytes + 1
        self.assertEqual(
            (over_max_payload_bytes + max_block_bytes - 1) // max_block_bytes,
            2,
        )
        self.assertIn("mem_service_qwen3_kv_state_block_span", source)
        self.assertIn("mem_service_qwen3_kv_state_alloc", source)
        self.assertIn("block_count =", source)
        self.assertIn("reserved_bytes = block_count * block_bytes", source)
        self.assertNotIn("kv_payload_len > MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES", source)

    def test_obmm_service_object_bytes_are_not_demo_named(self):
        source = SERVICE_C.read_text()

        self.assertIn("MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES", source)
        self.assertNotIn("MEM_SERVICE_OBMM_DEMO_OBJECT_BYTES", source)

    def test_qwen3_guest_runtime_kv_payload_grows_past_fixed_guard(self):
        source = GUEST_C.read_text()

        self.assertNotIn("W4_QWEN3_MAX_KV_PAYLOAD_BYTES", source)
        self.assertNotIn("qwen3 range kv payload too large", source)
        self.assertIn("uint8_t *kv_payload;", source)
        self.assertIn("kv_payload_capacity", source)
        self.assertIn("qwen3_range_runtime_forward_reserve_kv", source)
        self.assertIn("qwen3 range kv payload reserve failed", source)

    def test_w4_guest_legacy_kvcache_payload_is_not_demo_named(self):
        sim_uapi_source = SIM_UAPI_RS.read_text()
        sources = [
            GUEST_C.read_text(),
            FOUR_NODE_W4_RUNNER.read_text(),
            EIGHT_NODE_W4_RUNNER.read_text(),
            sim_uapi_source,
        ]
        combined = "\n".join(sources)

        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", sim_uapi_source)
        self.assertIn("legacy_kvcache_payload", combined)
        self.assertNotIn("W4_DEMO_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertNotIn("invalid_demo_kvcache_payload_bytes", combined)
        self.assertNotIn("legacy_demo_payload", combined)


if __name__ == "__main__":
    unittest.main()
