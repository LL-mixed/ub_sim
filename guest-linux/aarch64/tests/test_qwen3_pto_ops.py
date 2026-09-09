import json
from pathlib import Path
import subprocess
import sys
import unittest
import importlib.util


ROOT = Path(__file__).resolve().parents[3]


class Qwen3PtoOpsContractTest(unittest.TestCase):
    def test_cli_describes_the_complete_primitive_set_without_building(self):
        result = subprocess.run(
            [sys.executable, str(ROOT / "guest-linux/aarch64/scripts/run_qwen3_pto_ops_tests.py"),
             "--describe"], check=True, capture_output=True, text=True,
        )
        report = json.loads(result.stdout)
        self.assertEqual(report["memory_modes"], ["gm", "ub-gm"])
        self.assertEqual(set(report["operations"]), {
            "rms_norm", "linear", "rope", "attention", "softmax", "residual",
            "swiglu", "embedding", "kv_copy", "cast",
        })
        self.assertIn("no W5 E2E", report["scope"])

    def test_numerical_header_uses_pto_instructions_without_reference_scalar_math(self):
        source = (ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto/qwen3_pto_ops.hpp").read_text()
        for instruction in ("TLOAD", "TSTORE", "TCVT", "TMUL", "TROWSUM", "TSQRT",
                            "TEXP", "TROWMAX", "TROWEXPAND", "TDIV"):
            self.assertIn("pto::" + instruction + "(", source)
        for forbidden in ("std::exp(", "std::sqrt(", "std::memcpy(", ".GetValue("):
            self.assertNotIn(forbidden, source)

    def test_layer_graph_uses_complete_pto_path_and_separate_cache_views(self):
        source = (ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto/qwen3_pto_layer.hpp").read_text()
        for name in ("decoder_layer", "embedding", "terminal_logits", "previous_key",
                     "next_key", "rms_norm(", "linear(", "rope(", "attention(",
                     "residual(", "swiglu("):
            self.assertIn(name, source)
        for forbidden in ("std::exp(", "std::sqrt(", "std::memcpy(", ".GetValue("):
            self.assertNotIn(forbidden, source)

    def test_simpler_profile_reuses_the_numerical_header(self):
        path = ROOT / "guest-linux/aarch64/scripts/prepare_simpler_host_artifacts.py"
        spec = importlib.util.spec_from_file_location("qwen3_artifact_contract", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        profile = module.PROFILE_SPECS["host_qwen3_operator"]
        self.assertEqual(len(profile.args_template), 7)
        self.assertEqual(profile.orch_function, "build_qwen3_pto_operator_graph")
        self.assertEqual(module.resolve_example_root(Path("unused"), profile),
                         ROOT / "guest-linux/aarch64/libs/lingqu_shmem_pto")
        range_profile = module.PROFILE_SPECS["host_qwen3_range"]
        self.assertEqual(len(range_profile.args_template), 19)
        self.assertEqual(range_profile.orch_function, "build_qwen3_pto_range_graph")


if __name__ == "__main__":
    unittest.main()
