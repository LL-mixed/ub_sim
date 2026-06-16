import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_DIR = ROOT / "components" / "w5_mem_service"
SERVICE_C = SERVICE_DIR / "w4_kvcache_db_service.c"
SERVICE_H = SERVICE_DIR / "w4_kvcache_db_service.h"
GUEST_C = ROOT / "w4_guest_qemu_demo.c"


class W4DbRecordRecyclingTests(unittest.TestCase):
    def test_record_caps_support_long_decode_runs(self):
        header = SERVICE_H.read_text()
        source = SERVICE_C.read_text()

        max_records = re.search(r"#define W4_DB_MAX_RECORDS\s+(\d+)U", header)
        cluster_records = re.search(r"#define W4_DB_CLUSTER_MAX_RECORDS\s+(\d+)", source)

        self.assertIsNotNone(max_records)
        self.assertIsNotNone(cluster_records)
        self.assertGreaterEqual(int(max_records.group(1)), 1024)
        self.assertGreaterEqual(int(cluster_records.group(1)), 1024)

    def test_full_record_table_recycles_old_qwen3_runtime_records(self):
        source = SERVICE_C.read_text()

        self.assertIn("W4_DB_QWEN3_RECORD_RETAIN_STEPS", source)
        self.assertIn("w4_db_recycle_qwen3_runtime_record", source)
        self.assertIn('strstr(key, "decode-step")', source)
        self.assertIn('strstr(key, "/step/")', source)
        self.assertIn("rec = w4_db_alloc_record(svc);", source)
        self.assertIn("rec = w4_db_recycle_qwen3_runtime_record(svc, key);", source)

    def test_qwen3_kv_state_uses_tiered_block_spans(self):
        source = SERVICE_C.read_text()

        tier_names = [
            "W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES",
            "W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES",
            "W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES",
            "W4_DB_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES",
        ]
        tier_values = []

        for tier in tier_names:
            self.assertIn(tier, source)
            match = re.search(rf"#define {tier}\s+0x([0-9a-fA-F]+)ULL", source)
            if match:
                tier_values.append(int(match.group(1), 16))

        slot_bytes = re.search(
            r"#define W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES\s+0x([0-9a-fA-F]+)ULL",
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
        self.assertIn("w4_db_qwen3_kv_state_block_span", source)
        self.assertIn("w4_db_qwen3_kv_state_alloc", source)
        self.assertIn("block_count =", source)
        self.assertIn("reserved_bytes = block_count * block_bytes", source)
        self.assertNotIn("kv_payload_len > W4_DB_OBMM_QWEN3_KV_STATE_SLOT_BYTES", source)

    def test_qwen3_guest_runtime_kv_payload_grows_past_fixed_guard(self):
        source = GUEST_C.read_text()

        self.assertNotIn("W4_QWEN3_MAX_KV_PAYLOAD_BYTES", source)
        self.assertNotIn("qwen3 range kv payload too large", source)
        self.assertIn("uint8_t *kv_payload;", source)
        self.assertIn("kv_payload_capacity", source)
        self.assertIn("qwen3_range_runtime_forward_reserve_kv", source)
        self.assertIn("qwen3 range kv payload reserve failed", source)


if __name__ == "__main__":
    unittest.main()
