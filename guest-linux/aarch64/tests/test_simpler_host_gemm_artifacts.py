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
    def test_manifest_marks_current_simpler_capi_abi(self):
        producer = load_producer()
        self.assertEqual(producer.SIMPLER_CAPI_ABI_VERSION, 2)

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
        self.assertIn("orch_args.tensor(0)", orchestration_text)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration_text)
        self.assertIn("for (int k0 = 0; k0 < K; k0 += TileK)", kernel_text)
        self.assertIn("TMATMUL(c_tile", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)
        self.assertNotIn("TLOG", kernel_text)
        self.assertNotIn("TEXP", kernel_text)

    def test_quantized_profile_has_distinct_callable_and_integer_types(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_quantized_gemm"]
        self.assertEqual(profile.orch_function, "build_quantized_gemm_graph")
        self.assertEqual(profile.callable_hint, "host_quantized_gemm")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_quantized_gemm_orchestration(
                root, 128, 256, 384
            )
            kernel = producer.write_host_quantized_gemm_kernel(root, 128, 256, 384)
            orchestration_text = orchestration.read_text()
            kernel_text = kernel.read_text()

        self.assertIn("build_quantized_gemm_graph", orchestration_text)
        self.assertIn("sizeof(int8_t)", orchestration_text)
        self.assertIn("sizeof(int32_t)", orchestration_text)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration_text)
        self.assertIn("TileLeft<int8_t", kernel_text)
        self.assertIn("TileRight<int8_t", kernel_text)
        self.assertIn("TileAcc<int32_t", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)

    def test_fp32_profile_uses_float_inputs_and_distinct_callable(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_fp32_gemm"]
        self.assertEqual(profile.orch_function, "build_fp32_gemm_graph")
        self.assertEqual(profile.callable_hint, "host_fp32_gemm")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_fp32_gemm_orchestration(
                root, 128, 256, 128
            )
            kernel = producer.write_host_fp32_gemm_kernel(root, 128, 256, 128)
            orchestration_text = orchestration.read_text()
            kernel_text = kernel.read_text()

        self.assertIn("build_fp32_gemm_graph", orchestration_text)
        self.assertIn("sizeof(float)", orchestration_text)
        self.assertIn("TileLeft<float", kernel_text)
        self.assertIn("TileRight<float", kernel_text)
        self.assertIn("TileAcc<float", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)

    def test_fp8_profile_uses_mx_scales_and_a5_types(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_fp8_gemm"]
        self.assertEqual(profile.orch_function, "build_fp8_gemm_graph")
        self.assertEqual(profile.callable_hint, "host_fp8_gemm")
        self.assertEqual(
            [arg["name"] for arg in profile.args_template[:5]],
            [
                "activation_fp8",
                "weight_fp8",
                "activation_scale_ue8m0",
                "weight_scale_ue8m0",
                "output_fp32",
            ],
        )

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_fp8_gemm_orchestration(
                root, 128, 128, 128
            ).read_text()
            kernel = producer.write_host_fp8_gemm_kernel(
                root, 128, 128, 128
            ).read_text()

        self.assertIn("expected 5 tensors and 3 scalars", orchestration)
        self.assertIn("float8_e4m3_t", kernel)
        self.assertIn("float8_e8m0_t", kernel)
        self.assertIn("TileLeftScale<float8_e8m0_t", kernel)
        self.assertIn("TileRightScale<float8_e8m0_t", kernel)
        self.assertIn("TMATMUL_MX", kernel)
        self.assertNotIn("TMATMUL_ACC", kernel)

    def test_fp4_profile_lowers_packed_e2m1_to_full_k_mx_tiles(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_fp4_gemm"]
        self.assertEqual(profile.profile, "HostFp4Gemm")
        self.assertEqual(profile.orch_function, "build_fp4_gemm_graph")
        self.assertEqual(profile.callable_hint, "host_fp4_gemm")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_fp4_gemm_orchestration(
                root, 128, 4096, 128
            ).read_text()
            kernel = producer.write_host_fp4_gemm_kernel(
                root, 128, 4096, 128
            ).read_text()

        self.assertIn("build_fp4_gemm_graph", orchestration)
        self.assertIn("kM = 128", orchestration)
        self.assertIn("kK = 4096", orchestration)
        self.assertIn("float8_e4m3_t", kernel)
        self.assertIn("FullScaleK = K / 32", kernel)
        self.assertIn("for (int k0 = 0; k0 < K; k0 += TileK)", kernel)
        self.assertIn("TMATMUL_MX(c_tile, c_tile", kernel)
        self.assertNotIn("float4_e2m1x2_t", kernel)

    def test_q8_block_dot_profile_uses_partial_shape_int8_gemv(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_q8_block_dot"]
        self.assertEqual(profile.orch_function, "build_q8_block_dot_graph")
        self.assertEqual(profile.callable_hint, "host_q8_block_dot")

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_q8_block_dot_orchestration(root, 128, 1024)
            kernel = producer.write_host_q8_block_dot_kernel(root, 1024)
            orchestration_text = orchestration.read_text()
            kernel_text = kernel.read_text()

        self.assertIn("kBlocks = 128", orchestration_text)
        self.assertIn("kK = 32", orchestration_text)
        self.assertIn("kN = 1024", orchestration_text)
        self.assertIn("orch_args.scalar(0)", orchestration_text)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration_text)
        self.assertIn("TileLeft<int8_t, 1, K, 1, K>", kernel_text)
        self.assertIn("TileRight<int8_t, K, TileN, K, TileN>", kernel_text)
        self.assertIn("TGEMV(tile_c", kernel_text)
        self.assertIn("block * K * N", kernel_text)
        self.assertNotIn("TMATMUL_ACC", kernel_text)

    def test_engram_orchestration_uses_current_simpler_task_args(self):
        producer = load_producer()
        with tempfile.TemporaryDirectory() as temp_dir:
            orchestration = producer.write_host_engram_context_orchestration(
                Path(temp_dir)
            ).read_text()

        self.assertIn("orch_args.tensor(0)", orchestration)
        self.assertIn("orch_args.scalar(0)", orchestration)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration)


if __name__ == "__main__":
    unittest.main()
