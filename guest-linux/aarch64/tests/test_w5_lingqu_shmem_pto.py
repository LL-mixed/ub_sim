import importlib.util
import json
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
CLI = GUEST_ROOT / "scripts" / "run_w5_lingqu_shmem_pto.py"
RUNNER = GUEST_ROOT / "scripts" / "run_llm_infer_eight_node_guest.sh"
INITRAMFS_BUILDER = GUEST_ROOT / "scripts" / "build_initramfs.sh"
APP = GUEST_ROOT / "apps" / "llm_infer" / "llm_infer.c"
MAKEFILE = GUEST_ROOT / "apps" / "llm_infer" / "Makefile"


def load_cli_module():
    spec = importlib.util.spec_from_file_location("w5_lingqu_shmem_pto", CLI)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def write_callable_one_manifest(path: pathlib.Path) -> None:
    path.write_text(
        json.dumps(
            {
                "ub_gm_layout": {
                    "profile": "nd",
                    "pto_layout": "ND",
                    "compute_tile_layout": "RowMajor",
                    "rank": 5,
                    "shape": [1, 1, 1, 32, 32],
                    "strides": [1, 1, 1, 32, 1],
                    "global_rows": 32,
                    "global_cols": 32,
                    "tile_rows": 32,
                    "tile_cols": 32,
                    "logical_elements": 1024,
                    "logical_bytes": 4096,
                    "storage_elements": 1024,
                    "storage_bytes": 4096,
                    "fragments": [
                        {"element_offset": 0, "element_count": 1024}
                    ],
                },
                "simpler_runtime": {
                    "args_template": [
                        {"kind": "input", "name": "a"},
                        {"kind": "input", "name": "b"},
                        {"kind": "output", "name": "f"},
                    ],
                    "kernels": [{}, {}, {}],
                },
            }
        )
        + "\n"
    )


class W5LingquShmemPtoTest(unittest.TestCase):
    def test_cli_exposes_one_goal_oriented_entrypoint(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest = pathlib.Path(directory) / "manifest.json"
            write_callable_one_manifest(manifest)
            completed = subprocess.run(
                [
                    "python3",
                    str(CLI),
                    "--manifest",
                    str(manifest),
                    "--node-count",
                    "2",
                    "--print-plan",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            plan = json.loads(completed.stdout)
            self.assertEqual(plan["command"], "w5-lingqu-shmem-pto")
            self.assertEqual(plan["node_count"], 2)
            self.assertEqual(
                plan["implementation_stage"],
                "memory_service_hidden_ub_gm_in_place_publish",
            )
            self.assertIn("TLOAD/TSTORE", plan["acceptance_scope"])
            self.assertEqual(plan["access_bytes"], 4096)
            self.assertEqual(plan["hidden_bytes"], 262144)
            self.assertEqual(plan["decode_hidden_bytes"], 262144)
            self.assertEqual(plan["decode_tokens"], 128)
            self.assertEqual(plan["tile_count"], 64)
            self.assertTrue(plan["publish_output"])
            self.assertEqual(plan["decode_steps"], 2)
            self.assertEqual(plan["ub_gm_layout"]["shape"], [1, 1, 1, 32, 32])

    def test_cli_rejects_a_manifest_that_does_not_match_callable_one(self):
        module = load_cli_module()
        with tempfile.TemporaryDirectory() as directory:
            manifest = pathlib.Path(directory) / "manifest.json"
            write_callable_one_manifest(manifest)
            payload = json.loads(manifest.read_text())
            payload["ub_gm_layout"]["storage_bytes"] = 65536
            manifest.write_text(json.dumps(payload) + "\n")

            with self.assertRaisesRegex(RuntimeError, "canonical 32x32"):
                module.validate_manifest_contract(manifest)

    def test_runtime_matches_qwen_tp_width_to_selected_cluster(self):
        source = CLI.read_text()

        self.assertIn(
            '"SIM_QWEN3_DENSE_TP_NODES": str(args.node_count)',
            source,
        )
        self.assertIn(
            '"SIM_QWEN3_DENSE_DECODE_TOKENS": str(plan["decode_tokens"])',
            source,
        )
        self.assertIn(
            '"SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES": str(',
            source,
        )
        self.assertIn('plan["decode_hidden_bytes"]', source)

    def test_w5_guest_acquires_object_view_and_local_ub_gm_memrefs(self):
        source = APP.read_text()

        self.assertIn("run_w5_pto_ub_gm_hidden_transform", source)
        self.assertIn("lingqu_shmem_mem_service_acquire(", source)
        self.assertIn("lingqu_shmem_mem_service_acquire_local(", source)
        self.assertIn("lingqu_shmem_pto_dispatch_prepare(", source)
        self.assertIn("lingqu_shmem_pto_endpoint_submit(", source)
        self.assertIn("address_space=UB_GM guest_inline_payload=0", source)
        self.assertIn("tload=direct tstore=direct guest_inline_payload=0", source)
        self.assertIn("w5_pto_ub_gm_probe_semantic", source)
        self.assertIn("formula=(2*x+1)*(2*x+2)", source)
        self.assertIn("range_request.publish_payload_in_place = true", source)
        self.assertIn("w5_pto_ub_gm_hidden_publish", source)
        self.assertIn("SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT", source)
        self.assertIn("lingqu_shmem_pto_requester_cna", source)
        self.assertIn("fail W5 PTO UB_GM probe configuration", source)
        self.assertIn('probe_stage = "acquire_input"', source)
        self.assertIn('" stage=%s rc=%d completion=%s', source)
        self.assertIn("stage w5_pto_ub_gm_queue_handoff", source)
        handoff = source.index("stage w5_pto_ub_gm_queue_handoff")
        probe = source.index("run_w5_pto_ub_gm_hidden_transform(", handoff)
        self.assertLess(handoff, probe)

    def test_w5_binary_links_memory_service_compute_adapter(self):
        source = MAKEFILE.read_text()
        initramfs_builder = INITRAMFS_BUILDER.read_text()

        self.assertIn("lingqu_shmem_mem_service.c", source)
        self.assertIn("lingqu_shmem_mem_service_obmm.c", source)
        self.assertIn("lingqu_shmem_pto_endpoint.c", source)
        self.assertIn("obmm_async.c", source)
        self.assertIn('"$LINGQU_SHMEM_MEM_SERVICE_SRC"', initramfs_builder)
        self.assertIn('"$LINGQU_SHMEM_MEM_SERVICE_OBMM_SRC"', initramfs_builder)
        self.assertIn('"$OBMM_ASYNC_COROUTINE_LIB_SRC"', initramfs_builder)

    def test_runner_enables_per_node_pto_and_checks_zero_staging(self):
        source = RUNNER.read_text()

        self.assertIn("validate_w5_pto_ub_gm_probe", source)
        self.assertIn("SIM_LINGQU_SHMEM_PTO_ENABLE", source)
        self.assertIn("SIM_LINGQU_SHMEM_PTO_CNA_BASE", source)
        self.assertIn("SIMPLER_HOST_VECTOR_MANIFEST", source)
        self.assertIn("QEMU_UB_GM_LOAD", source)
        self.assertIn("QEMU_UB_GM_STORE", source)
        self.assertIn("segment_payload_staging_bytes=0", source)
        self.assertIn("per-run initramfs build failed", source)
        self.assertIn("per-run initramfs output missing", source)
        self.assertIn("SIM_W5_PTO_UB_GM_DISABLE_EXPERIMENTAL_GSVA", source)
        self.assertIn("SIM_W5_PTO_UB_GM_ACCESS_BYTES", source)
        self.assertIn("SIM_W5_PTO_UB_GM_PUBLISH_OUTPUT", source)
        self.assertIn("backend=ub_ssd_gsva enabled=0", source)
        self.assertIn("payload_mode=(copy|in_place)", source)

    def test_structured_validator_requires_tload_tstore_and_zero_staging(self):
        module = load_cli_module()
        with tempfile.TemporaryDirectory() as directory:
            guest_root = pathlib.Path(directory) / "aarch64"
            run_id = "unit-w5-pto"
            run_dir = guest_root / "logs" / f"{run_id}_headless8"
            run_dir.mkdir(parents=True)
            module.GUEST_ROOT = guest_root

            (run_dir / "nodeA_guest.log").write_text(
                "stage w5_pto_ub_gm_probe_config status=ok\n"
                "stage w5_pto_ub_gm_probe_skip status=skipped\n"
            )
            (run_dir / "nodeA_qemu.log").write_text("")
            (run_dir / "nodeB_guest.log").write_text(
                "stage w5_pto_ub_gm_probe_config status=ok\n"
                "stage w5_pto_ub_gm_probe_submit status=ready\n"
                "stage w5_pto_ub_gm_probe_semantic elements=1024 "
                "formula=(2*x+1)*(2*x+2) status=ok\n"
                "stage w5_pto_ub_gm_probe_complete status=ok\n"
            )
            qemu_log = run_dir / "nodeB_qemu.log"
            qemu_log.write_text(
                "QEMU_UB_GM_LOAD request=2 binding=1 addr=0x1000 "
                "length=4096 map=1 generation=1\n"
                "QEMU_UB_GM_LOAD request=2 binding=2 addr=0x1000 "
                "length=4096 map=1 generation=1\n"
                "QEMU_UB_GM_STORE request=2 binding=3 addr=0x2000 "
                "length=4096 map=2 generation=2\n"
                "QEMU_UB_GM_UNBIND load_bytes=8192 store_bytes=4096 "
                "fences=1 segment_payload_staging_bytes=0\n"
            )
            report = module.collect_validation(
                run_id, 2, 0, "0x1234", "0x1234", 4096, 4096, 1, False
            )
            self.assertEqual(report["status"], "pass")

            qemu_log.write_text(
                "QEMU_UB_GM_LOAD request=2 binding=1 addr=0x1000 "
                "length=4096 map=1 generation=1\n"
                "QEMU_UB_GM_LOAD request=2 binding=2 addr=0x1000 "
                "length=4096 map=1 generation=1\n"
            )
            report = module.collect_validation(
                run_id, 2, 0, "0x1234", "0x1234", 4096, 4096, 1, False
            )
            self.assertEqual(report["status"], "fail")
            self.assertEqual(report["nodes"][1]["tstore_count"], 0)

    def test_structured_validator_requires_full_hidden_in_place_publish(self):
        module = load_cli_module()
        with tempfile.TemporaryDirectory() as directory:
            guest_root = pathlib.Path(directory) / "aarch64"
            run_id = "unit-w5-pto-publish"
            run_dir = guest_root / "logs" / f"{run_id}_headless8"
            run_dir.mkdir(parents=True)
            module.GUEST_ROOT = guest_root

            (run_dir / "nodeA_guest.log").write_text(
                "stage w5_pto_ub_gm_probe_config status=ok\n"
                "stage w5_pto_ub_gm_probe_skip status=skipped\n"
                "stage w5_pto_ub_gm_probe_skip status=skipped\n"
                "stage w5_pto_ub_gm_hidden_publish tiles=0 "
                "publish_mode=copy status=ok\n"
                "stage w5_pto_ub_gm_hidden_publish tiles=0 "
                "publish_mode=copy status=ok\n"
            )
            (run_dir / "nodeA_qemu.log").write_text("")
            node_b_lines = ["stage w5_pto_ub_gm_probe_config status=ok"]
            qemu_lines = []
            for step in range(2):
                node_b_lines.extend(
                    [
                        "stage w5_pto_ub_gm_hidden_transform_complete "
                        f"step={step} bytes=8192 tile_bytes=4096 tiles=2 status=ok",
                        "stage w5_pto_ub_gm_hidden_publish "
                        f"step={step} bytes=8192 tiles=2 "
                        "publish_mode=in_place status=ok",
                    ]
                )
                for tile in range(2):
                    request = step * 2 + tile
                    node_b_lines.extend(
                        [
                            "stage w5_pto_ub_gm_probe_submit "
                            f"step={step} tile={tile} status=ready",
                            "stage w5_pto_ub_gm_probe_semantic elements=1024 "
                            "formula=(2*x+1)*(2*x+2) status=ok",
                            "stage w5_pto_ub_gm_probe_complete "
                            f"step={step} tile={tile} status=ok",
                        ]
                    )
                    qemu_lines.extend(
                        [
                            f"QEMU_UB_GM_LOAD request={request} "
                            "length=4096 map=1",
                            f"QEMU_UB_GM_LOAD request={request} "
                            "length=4096 map=1",
                            f"QEMU_UB_GM_STORE request={request} "
                            "length=4096 map=2",
                            "QEMU_UB_GM_UNBIND load_bytes=8192 "
                            "store_bytes=4096 fences=1 "
                            "segment_payload_staging_bytes=0",
                        ]
                    )
            (run_dir / "nodeB_guest.log").write_text(
                "\n".join(node_b_lines) + "\n"
            )
            (run_dir / "nodeB_qemu.log").write_text(
                "\n".join(qemu_lines) + "\n"
            )

            report = module.collect_validation(
                run_id, 2, 0, "0x1234", "0x1234", 4096, 8192, 2, True
            )
            self.assertEqual(report["status"], "pass")
            self.assertEqual(report["nodes"][1]["semantic_count"], 4)
            self.assertEqual(report["nodes"][1]["in_place_publish_count"], 2)

            node_b_lines.pop()
            (run_dir / "nodeB_guest.log").write_text(
                "\n".join(node_b_lines) + "\n"
            )
            report = module.collect_validation(
                run_id, 2, 0, "0x1234", "0x1234", 4096, 8192, 2, True
            )
            self.assertEqual(report["status"], "fail")


if __name__ == "__main__":
    unittest.main()
