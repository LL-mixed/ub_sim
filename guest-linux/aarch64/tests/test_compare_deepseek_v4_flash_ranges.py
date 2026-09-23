import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("compare_deepseek_v4_flash_ranges.py")
SPEC = importlib.util.spec_from_file_location("deepseek_range_compare", SCRIPT)
COMPARE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(COMPARE)


class DeepseekRangeCompareTest(unittest.TestCase):
    def test_reads_f32_and_reports_elementwise_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "values.f32"
            path.write_bytes(struct.pack("<3f", 1.0, -2.0, 3.0))
            self.assertEqual(COMPARE.read_f32(path), [1.0, -2.0, 3.0])
            self.assertEqual(COMPARE.read_f32(path, 4, 1), [-2.0])
            with self.assertRaises(ValueError):
                COMPARE.read_f32(path, 8, 2)
        result = COMPARE.compare([1.0, -2.0, 3.0], [1.0, -1.9, 3.0], 0.01, 0.01)
        self.assertEqual(result["elements"], 3)
        self.assertEqual(result["exact_elements"], 2)
        self.assertEqual(result["outside_tolerance"], 1)
        self.assertEqual(result["max_abs_index"], 1)
        self.assertAlmostEqual(result["max_abs_error"], 0.1)

    def test_rejects_bad_geometry_and_nonfinite_values(self):
        with self.assertRaises(ValueError):
            COMPARE.compare([1.0], [], 0.0, 0.0)
        with self.assertRaises(ValueError):
            COMPARE.compare([1.0], [float("nan")], 0.0, 0.0)
        with self.assertRaises(ValueError):
            COMPARE.compare([1.0], [1.0], -1.0, 0.0)


if __name__ == "__main__":
    unittest.main()
