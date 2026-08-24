import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
LLM_INFER_SOURCE = ROOT / "apps" / "llm_infer" / "llm_infer.c"
FOUR_NODE_W4_RUNNER = ROOT / "scripts" / "run_ub_four_node_w4_guest.sh"
EIGHT_NODE_W4_RUNNER = ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh"
SIM_UAPI_SOURCE = ROOT.parents[1] / "crates" / "sim-uapi" / "src" / "lib.rs"
APP_BUILD_MATRIX = ROOT / "scripts" / "run_ub_app_build_matrix.sh"
INITRAMFS_BUILDER = ROOT / "scripts" / "build_initramfs.sh"
SOURCE_VERIFIER = ROOT / "scripts" / "verify_mem_service_source.py"
SOURCE_LOCK = ROOT / "mem_service.lock"
MEM_SERVICE_ROOT = REPO_ROOT / "mem_service"
GITMODULES = REPO_ROOT / ".gitmodules"


class MemServiceDownstreamContractTests(unittest.TestCase):
    def _verify_source(self, lock_file: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                "python3",
                str(SOURCE_VERIFIER),
                "--mem-service-root",
                str(MEM_SERVICE_ROOT),
                "--lock-file",
                str(lock_file),
            ],
            cwd=ROOT,
            capture_output=True,
            text=True,
        )

    def test_source_lock_accepts_pinned_clean_checkout(self):
        result = self._verify_source(SOURCE_LOCK)
        revision = next(
            line.split("=", 1)[1]
            for line in SOURCE_LOCK.read_text().splitlines()
            if line.startswith("revision=")
        )

        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertIn("mem_service_source_check=ok version=0.1.0", result.stdout)
        self.assertIn(f"revision={revision}", result.stdout)

    def test_source_lock_matches_mem_service_submodule(self):
        gitmodules = GITMODULES.read_text()
        self.assertIn('[submodule "mem_service"]', gitmodules)
        self.assertIn("path = mem_service", gitmodules)
        self.assertIn(
            "url = https://github.com/LL-mixed/mem_service",
            gitmodules,
        )

        lock_revision = next(
            line.split("=", 1)[1]
            for line in SOURCE_LOCK.read_text().splitlines()
            if line.startswith("revision=")
        )
        result = subprocess.run(
            ["git", "ls-files", "--stage", "mem_service"],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        mode, gitlink_revision, stage_and_path = result.stdout.split(maxsplit=2)
        self.assertEqual(mode, "160000")
        self.assertEqual(stage_and_path, "0\tmem_service\n")
        self.assertEqual(gitlink_revision, lock_revision)

    def test_build_entrypoints_default_to_mem_service_submodule(self):
        paths = (
            ROOT / "apps" / "llm_infer" / "Makefile",
            ROOT / "apps" / "serving_control" / "Makefile",
            ROOT / "apps" / "pretraining_client" / "Makefile",
            APP_BUILD_MATRIX,
            INITRAMFS_BUILDER,
            ROOT / "scripts" / "run_w5_memory_service_bootstrap.sh",
        )
        for path in paths:
            with self.subTest(path=path):
                source = path.read_text()
                self.assertIn("../../mem_service", source)
                self.assertNotIn("../../../mem_service", source)

    def test_source_lock_rejects_incompatible_version_and_revision(self):
        with tempfile.TemporaryDirectory(prefix="ub-sim-mem-service-lock-") as tmp:
            lock_file = Path(tmp) / "mem_service.lock"
            lock_file.write_text(
                "lock_version=1\n"
                "version=0.2.0\n"
                "revision=0000000000000000000000000000000000000000\n"
            )

            result = self._verify_source(lock_file)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "incompatible mem_service source version",
                result.stderr,
            )

    def test_app_build_matrix_builds_qwen3_adapter(self):
        source = APP_BUILD_MATRIX.read_text()

        self.assertIn("verify_mem_service_source.py", source)
        self.assertIn(
            'make -C "$app_dir" LLM_INFER_ROOT="$ROOT_DIR" || rc=$?',
            source,
        )
        self.assertIn(
            'make -C "$app_dir" clean LLM_INFER_ROOT="$ROOT_DIR" || clean_rc=$?',
            source,
        )

    def test_initramfs_signature_tracks_mem_service_revision_and_lock(self):
        source = INITRAMFS_BUILDER.read_text()

        self.assertIn("mem_service_head=", source)
        self.assertIn(
            'git -C "$MEM_SERVICE_ROOT" rev-parse HEAD',
            source,
        )
        self.assertIn(
            'write_signature_line "mem_service_lock" '
            '"$ROOT_DIR/mem_service.lock"',
            source,
        )

    def test_model_range_completion_acceptance_is_not_w5_only(self):
        source = EIGHT_NODE_W4_RUNNER.read_text()

        self.assertIn(
            'if is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; '
            "then",
            source,
        )
        self.assertNotIn(
            'if [[ -n "$SIM_UAPI_W5_PROFILE" ]] && '
            'is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; '
            "then",
            source,
        )

    def test_llm_infer_internal_memory_symbols_are_not_w5_named(self):
        source = LLM_INFER_SOURCE.read_text()

        self.assertNotIn("W4_QWEN3_W5_", source)
        self.assertNotIn("parse_qwen3_w5_", source)
        self.assertNotIn("qwen3_read_w5_", source)
        self.assertNotIn("qwen3_w5_memory_service_lookup_boundary", source)
        self.assertIn("QWEN3_MEMORY_SHORTPATH_STREAM_MAX", source)
        self.assertIn("parse_qwen3_memory_decision_config", source)
        self.assertIn("qwen3_memory_service_lookup_boundary", source)

    def test_model_range_runtime_kv_payload_grows_past_fixed_guard(self):
        source = LLM_INFER_SOURCE.read_text()

        self.assertNotIn("W4_QWEN3_MAX_KV_PAYLOAD_BYTES", source)
        self.assertNotIn("qwen3 range kv payload too large", source)
        self.assertIn("uint8_t *kv_payload;", source)
        self.assertIn("kv_payload_capacity", source)
        self.assertIn("model_range_runtime_forward_reserve_kv", source)
        self.assertIn("model range kv payload reserve failed", source)

    def test_w4_guest_legacy_kvcache_payload_is_not_demo_named(self):
        sim_uapi_source = SIM_UAPI_SOURCE.read_text()
        combined = "\n".join(
            [
                LLM_INFER_SOURCE.read_text(),
                FOUR_NODE_W4_RUNNER.read_text(),
                EIGHT_NODE_W4_RUNNER.read_text(),
                sim_uapi_source,
            ]
        )

        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", sim_uapi_source)
        self.assertIn("legacy_kvcache_payload", combined)
        self.assertNotIn("W4_DEMO_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertNotIn("invalid_demo_kvcache_payload_bytes", combined)
        self.assertNotIn("legacy_demo_payload", combined)


if __name__ == "__main__":
    unittest.main()
