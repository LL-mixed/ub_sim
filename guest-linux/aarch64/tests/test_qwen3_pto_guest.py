import pathlib
import importlib.util
import json
import struct
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST = ROOT / "guest-linux/aarch64"
LIB = GUEST / "libs/lingqu_shmem_pto"


class Qwen3PtoGuestTest(unittest.TestCase):
    def test_token_table_preserves_raw_piece_and_padded_vocabulary(self):
        spec = importlib.util.spec_from_file_location("pto_token_table", GUEST / "scripts/prepare_qwen3_pto_token_table.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory)
            (path / "config.json").write_text(json.dumps({"vocab_size": 4}))
            (path / "tokenizer.json").write_text(json.dumps({"model": {"vocab": {"Hi": 0, "Ġworld": 1}},
                                                          "added_tokens": [{"id": 2, "content": "<end>"}]}))
            data = module.token_table(path)
            self.assertEqual(struct.unpack_from("<4Q", data), (0x515750544F545854, 1, 4, 32))
            self.assertEqual(len(data), 160)
            self.assertEqual(data[-32:], bytes(32))
            self.assertEqual(struct.unpack_from("<Q", data, 72)[0], len("Ġworld".encode()))
            self.assertEqual(data[80:96].rstrip(b"\0"), "Ġworld".encode())

    def test_model_runner_plan_and_guest_branch(self):
        result = subprocess.run(["python3", str(GUEST / "scripts/run_w5_qwen3_pto.py"),
                                 "--manifest", "m.json", "--weights", "weights", "--scenario", "s.yaml",
                                 "--run-id", "contract-model-pto", "--print-plan"],
                                check=True, capture_output=True, text=True)
        plan = json.loads(result.stdout)
        self.assertEqual((plan["nodes"], plan["steps"], plan["callable"]), (2, 8, 3))
        self.assertFalse(plan["numerical_trace"])
        traced = subprocess.run(["python3", str(GUEST / "scripts/run_w5_qwen3_pto.py"),
                                "--manifest", "m.json", "--weights", "weights", "--scenario", "s.yaml",
                                "--run-id", "contract-model-trace", "--numerical-trace", "--print-plan"],
                               check=True, capture_output=True, text=True)
        self.assertTrue(json.loads(traced.stdout)["numerical_trace"])
        source = (GUEST / "apps/llm_infer/llm_infer.c").read_text()
        branch = source[source.index("    if (w5_qwen3_pto) {\n        struct qwen3_pto_guest_geometry"):
                        source.index("    base_submit_ms = monotonic_ms();", source.index("    compute_start_ms = monotonic_ms();"))]
        self.assertIn("qwen3_pto_guest_submit", branch)
        self.assertIn("goto model_pto_ready", branch)
        self.assertNotIn("verify_dispatch_payload", branch)
        self.assertIn("model_pto_terminal_record(&qwen3_pto_output", source)
        self.assertIn("qwen3_pto_previous_kv = previous_kv_view", source)
        runner = (GUEST / "scripts/run_llm_infer_eight_node_guest.sh").read_text()
        self.assertIn('publisher=terminal_node" "$SIM_QWEN3_GUEST_DECODE_STEPS"', runner)
        model_runner = (GUEST / "scripts/run_w5_qwen3_pto.py").read_text()
        self.assertIn('"SIM_W5_PTO_UB_GM_DISABLE_EXPERIMENTAL_GSVA": "1"', model_runner)

    def test_model_prepare_completion_and_retained_timeout_leases(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = pathlib.Path(directory) / "qwen3-guest-golden"
            command = ["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-I", str(ROOT / "mem_service"), "-I", str(LIB),
                       "-I", str(ROOT / "crates/sim-qemu/include"),
                       str(GUEST / "tests/qwen3_pto_guest_golden.c"),
                       str(LIB / "lingqu_shmem_mem_service.c"),
                       str(LIB / "lingqu_shmem.c"),
                       str(LIB / "lingqu_shmem_pto_guest.c"), "-lm", "-o", str(binary)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertIn("qwen3_pto_guest_golden=pass", result.stdout)

    def test_both_w5_build_entries_include_model_dispatch(self):
        self.assertIn("/qwen3_pto_guest.c", (GUEST / "apps/llm_infer/Makefile").read_text())
        build = (GUEST / "scripts/build_initramfs.sh").read_text()
        self.assertIn('write_signature_line "qwen3_pto_guest_src"', build)
        self.assertIn('"$LLM_INFER_APP_SRC" "$QWEN3_PTO_GUEST_SRC"', build)
        self.assertIn('local llm_infer_pto_sources=(\n    "$QWEN3_PTO_GUEST_SRC"', build)


if __name__ == "__main__":
    unittest.main()
