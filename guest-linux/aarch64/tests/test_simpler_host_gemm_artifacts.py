import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
PRODUCER = REPO_ROOT / "guest-linux/aarch64/scripts/prepare_simpler_host_artifacts.py"


def load_producer():
    spec = importlib.util.spec_from_file_location("prepare_simpler_host_artifacts", PRODUCER)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class SimplerHostGemmArtifactsTest(unittest.TestCase):
    def test_profile_is_pure_gemm_with_explicit_geometry(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_gemm"]
        self.assertEqual(profile.orch_function, "build_gemm_graph")
        self.assertEqual(profile.callable_hint, "host_gemm")
        self.assertEqual([arg["name"] for arg in profile.args_template], ["a", "b", "c", "m", "k", "n"])

    def test_generated_kernel_tiles_and_accumulates_k_dimension(self):
        producer = load_producer()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_gemm_orchestration(root, 128, 256, 384)
            kernel = producer.write_host_gemm_kernel(root, 128, 256, 384)
            orchestration_text = orchestration.read_text()
            kernel_text = kernel.read_text()

        self.assertIn("kM = 128", orchestration_text)
        self.assertIn("kK = 256", orchestration_text)
        self.assertIn("kN = 384", orchestration_text)
        self.assertIn("for (int k0 = 0; k0 < K; k0 += TileK)", kernel_text)
        self.assertIn("TMATMUL(c_tile", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)
        self.assertNotIn("TLOG", kernel_text)
        self.assertNotIn("TEXP", kernel_text)


if __name__ == "__main__":
    unittest.main()
