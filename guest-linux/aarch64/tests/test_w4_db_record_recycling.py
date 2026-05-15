import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SERVICE_C = ROOT / "w4_kvcache_db_service.c"
SERVICE_H = ROOT / "w4_kvcache_db_service.h"


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


if __name__ == "__main__":
    unittest.main()
