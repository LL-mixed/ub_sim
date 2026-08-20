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
    def assert_current_chip_tensor_kernel(self, kernel, tensor_names):
        self.assertIn('#include "tensor.h"', kernel)
        for name in tensor_names:
            self.assertIn(f"ChipTensor* {name}_tensor", kernel)
            self.assertIn(f"{name}_tensor->buffer.addr", kernel)
            self.assertIn(f"{name}_tensor->start_offset", kernel)

    def test_manifest_marks_current_simpler_capi_abi(self):
        producer = load_producer()
        self.assertEqual(producer.SIMPLER_CAPI_ABI_VERSION, 3)

    def test_reuse_manifest_rejects_stale_simpler_capi_abi(self):
        producer = load_producer()
        with tempfile.TemporaryDirectory() as temp_dir:
            manifest_path = Path(temp_dir) / "manifest.json"
            manifest_path.write_text(
                '{"simpler_capi_abi_version": 2, "simpler_runtime": {}}'
            )
            with self.assertRaisesRegex(SystemExit, "stale simpler C API ABI"):
                producer.load_reuse_runtime_manifest(manifest_path)

    def test_producer_uses_simpler_runtime_incore_include_contract(self):
        source = PRODUCER.read_text()
        self.assertIn(
            "kernel_compiler.get_orchestration_include_dirs", source
        )
        self.assertIn("extra_include_dirs=incore_include_dirs", source)

    def test_static_libgcc_wrapper_is_owned_by_artifact_producer(self):
        producer = load_producer()

        class Toolchain:
            cxx_path = ""

        class Compiler:
            gxx15 = Toolchain()

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            compiler_path = root / "g++-15"
            compiler_path.write_text("#!/bin/sh\nexit 0\n")
            compiler_path.chmod(0o755)
            compiler = Compiler()
            compiler.gxx15.cxx_path = str(compiler_path)

            wrapper = producer.configure_sim_kernel_libgcc(
                compiler, root / "build", "static"
            )

            self.assertIsNotNone(wrapper)
            wrapper_text = wrapper.read_text()
            self.assertIn(str(compiler_path), wrapper_text)
            self.assertIn("-static-libgcc", wrapper_text)
            self.assertEqual(compiler.gxx15.cxx_path, str(wrapper))

    def test_shared_libgcc_mode_preserves_simpler_toolchain(self):
        producer = load_producer()

        class Toolchain:
            cxx_path = "g++-15"

        class Compiler:
            gxx15 = Toolchain()

        compiler = Compiler()
        wrapper = producer.configure_sim_kernel_libgcc(
            compiler, Path("unused"), "shared"
        )

        self.assertIsNone(wrapper)
        self.assertEqual(compiler.gxx15.cxx_path, "g++-15")

    def test_producer_exposes_sim_kernel_libgcc_cli(self):
        source = PRODUCER.read_text()
        self.assertIn('"--sim-kernel-libgcc"', source)
        self.assertIn('choices=("static", "shared")', source)
        self.assertIn('default="static"', source)

    def test_standard_profiles_use_exported_orchestration_entry(self):
        producer = load_producer()
        vector = producer.PROFILE_SPECS["host_vector"]
        matmul = producer.PROFILE_SPECS["host_matmul"]
        self.assertEqual(vector.orch_function, "aicpu_orchestration_entry")
        self.assertEqual(matmul.orch_function, "aicpu_orchestration_entry")
        self.assertEqual(len(vector.args_template), 3)
        self.assertEqual(len(matmul.args_template), 4)

    def test_generated_vector_kernels_use_current_chip_tensor_abi(self):
        producer = load_producer()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            add = producer.write_vector_kernel_source(root, 0, 32, 32).read_text()
            add_scalar = producer.write_vector_kernel_source(
                root, 1, 32, 32
            ).read_text()

        self.assertIn('#include "tensor.h"', add)
        self.assertIn("ChipTensor* src0_tensor", add)
        self.assertIn("ChipTensor* src1_tensor", add)
        self.assertIn("ChipTensor* out_tensor", add)
        self.assertIn("converter.u64 = args[2]", add_scalar)
        self.assertNotIn("int size =", add)
        self.assertNotIn("int size =", add_scalar)

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
        self.assertIn("aicpu_orchestration_config", orchestration_text)
        self.assertIn("rt_submit_aic_task", orchestration_text)
        self.assertNotIn("OrchestrationRuntime", orchestration_text)
        self.assertNotIn("device_malloc", orchestration_text)
        self.assertIn("for (int k0 = 0; k0 < K; k0 += TileK)", kernel_text)
        self.assertIn("TMATMUL(c_tile", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)
        self.assertNotIn("TLOG", kernel_text)
        self.assertNotIn("TEXP", kernel_text)
        self.assert_current_chip_tensor_kernel(kernel_text, ("a", "b", "c"))

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
        self.assertIn("expected_arg_count = 6", orchestration_text)
        self.assertIn("task_args.add_output(c)", orchestration_text)
        self.assertIn("rt_submit_aic_task", orchestration_text)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration_text)
        self.assertIn("TileLeft<int8_t", kernel_text)
        self.assertIn("TileRight<int8_t", kernel_text)
        self.assertIn("TileAcc<int32_t", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)
        self.assert_current_chip_tensor_kernel(kernel_text, ("a", "b", "c"))

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
        self.assertIn("expected_arg_count = 6", orchestration_text)
        self.assertIn("rt_submit_aic_task", orchestration_text)
        self.assertIn("TileLeft<float", kernel_text)
        self.assertIn("TileRight<float", kernel_text)
        self.assertIn("TileAcc<float", kernel_text)
        self.assertIn("TMATMUL_ACC(c_tile", kernel_text)
        self.assert_current_chip_tensor_kernel(kernel_text, ("a", "b", "c"))

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

        self.assertIn("expected 5 tensor args and 3 scalar args", orchestration)
        self.assertIn("expected_arg_count = 8", orchestration)
        self.assertIn("rt_submit_aic_task", orchestration)
        self.assertIn("float8_e4m3_t", kernel)
        self.assertIn("float8_e8m0_t", kernel)
        self.assertIn("TileLeftScale<float8_e8m0_t", kernel)
        self.assertIn("TileRightScale<float8_e8m0_t", kernel)
        self.assertIn("TMATMUL_MX", kernel)
        self.assertNotIn("TMATMUL_ACC", kernel)
        self.assert_current_chip_tensor_kernel(
            kernel,
            (
                "activation",
                "weight",
                "activation_scale",
                "weight_scale",
                "output",
            ),
        )

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
        self.assert_current_chip_tensor_kernel(
            kernel,
            (
                "activation",
                "weight",
                "activation_scale",
                "weight_scale",
                "output",
            ),
        )

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
        self.assertIn("expected_arg_count = 6", orchestration_text)
        self.assertIn("rt_submit_aic_task", orchestration_text)
        self.assertIn("TileLeft<int8_t, 1, K, 1, K>", kernel_text)
        self.assertIn("TileRight<int8_t, K, TileN, K, TileN>", kernel_text)
        self.assertIn("TGEMV(tile_c", kernel_text)
        self.assertIn("block * K * N", kernel_text)
        self.assertNotIn("TMATMUL_ACC", kernel_text)
        self.assert_current_chip_tensor_kernel(
            kernel_text, ("activation", "weight", "output")
        )

    def test_engram_orchestration_uses_current_simpler_task_args(self):
        producer = load_producer()
        self.assertEqual(
            producer.PROFILE_SPECS["host_engram_context"].args_template[4]["kind"],
            "inout",
        )
        with tempfile.TemporaryDirectory() as temp_dir:
            orchestration = producer.write_host_engram_context_orchestration(
                Path(temp_dir)
            ).read_text()

        self.assertIn("orch_args.tensor(0)", orchestration)
        self.assertIn("orch_args.scalar(0)", orchestration)
        self.assertIn("expected_arg_count = 12", orchestration)
        self.assertIn("get_tensor_data<float>", orchestration)
        self.assertIn("set_tensor_data(output", orchestration)
        self.assertIn("rt_submit_aiv_task", orchestration)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration)
        self.assertNotIn("OrchestrationRuntime", orchestration)

    def test_batched_matmul_uses_current_simpler_task_graph(self):
        producer = load_producer()
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_batched_matmul_orchestration(
                root, 4
            ).read_text()
            kernels = [
                producer.write_batched_matmul_kernel_source(root, func_id, 4)
                .read_text()
                for func_id in range(3)
            ]

        self.assertIn("expected_arg_count = 10", orchestration)
        self.assertIn("aicpu_orchestration_entry", orchestration)
        self.assertIn("TensorCreateInfo b_info", orchestration)
        self.assertIn("rt_submit_aiv_task", orchestration)
        self.assertIn("rt_submit_aic_task", orchestration)
        self.assertNotIn("OrchestrationRuntime", orchestration)
        self.assertNotIn("device_malloc", orchestration)
        self.assertNotIn("add_successor", orchestration)
        self.assert_current_chip_tensor_kernel(kernels[0], ("src", "out"))
        self.assert_current_chip_tensor_kernel(
            kernels[1], ("src0", "src1", "out")
        )
        self.assert_current_chip_tensor_kernel(
            kernels[2], ("src0", "src1", "out")
        )

    def test_deepseek_vector_profile_uses_a5_kernel_dispatch(self):
        producer = load_producer()
        profile = producer.PROFILE_SPECS["host_deepseek_vector"]
        self.assertEqual(profile.profile, "HostVector")
        self.assertEqual(profile.orch_function, "build_deepseek_vector_graph")
        self.assertEqual(profile.callable_hint, "host_deepseek_vector")
        self.assertEqual(len(profile.args_template), 15)

        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            orchestration = producer.write_host_deepseek_vector_orchestration(
                root
            ).read_text()
            kernel = producer.write_host_deepseek_vector_kernel(root).read_text()

        self.assertIn("expected 4 tensor args and 11 scalar args", orchestration)
        self.assertIn("expected_arg_count = 15", orchestration)
        self.assertIn("rt_submit_aiv_task", orchestration)
        self.assertIn("orch_args.tensor(3)", orchestration)
        self.assertIn("orch_args.scalar(index)", orchestration)
        self.assertNotIn("CompatChipStorageTaskArgs", orchestration)
        for operation in (
            "RMS_NORM",
            "HC_SPLIT",
            "HC_WEIGHTED_SUM",
            "HC_POST",
            "ROPE",
            "KV_FP8_ROUNDTRIP",
            "SINK_ATTENTION",
            "INDEXER_QAT",
            "SCALE",
            "SWIGLU",
            "ADD",
            "ROUTER",
            "TOP_K",
            "HC_HEAD_WEIGHTS",
            "COMPRESSOR_POOL",
        ):
            self.assertIn(operation, kernel)
        self.assertIn("round_bf16", kernel)
        self.assertIn("float scores[1024]", kernel)
        self.assertNotIn("reference", kernel.lower())
        self.assertNotIn("fallback", kernel.lower())
        self.assert_current_chip_tensor_kernel(
            kernel, ("input0", "input1", "input2", "output")
        )


if __name__ == "__main__":
    unittest.main()
