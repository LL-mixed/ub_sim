#!/usr/bin/env python3

import os
import subprocess
import unittest
from pathlib import Path


class W5ClusterTopologyTest(unittest.TestCase):
    def setUp(self):
        self.repo = Path(__file__).resolve().parents[3]
        self.script_dir = self.repo / "guest-linux" / "aarch64" / "scripts"
        self.config_runner = self.script_dir / "run_w5_cluster_config.sh"
        self.guest_runner = self.script_dir / "run_llm_infer_eight_node_guest.sh"
        self.launcher = self.script_dir / "launch_ub_eight_node_headless.sh"
        self.infer_source = (
            self.repo / "guest-linux" / "aarch64" / "apps" / "llm_infer" / "llm_infer.c"
        )
        self.mem_service_cluster_runtime = (
            Path(os.environ.get("MEM_SERVICE_ROOT", self.repo / "mem_service"))
            / "components"
            / "mem_service"
            / "mem_service_cluster_runtime.c"
        )
        self.deepseek_config = self.repo / "w5.deepseek-v4-flash-simpler.env"

    def run_config(self, *args):
        return subprocess.run(
            [str(self.config_runner), *args, str(self.deepseek_config)],
            cwd=self.repo,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def test_cli_selects_supported_cluster_sizes(self):
        for node_count in (2, 3, 4, 8):
            with self.subTest(node_count=node_count):
                result = self.run_config(
                    "--print-env", "--nodes", str(node_count)
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(
                    f"SIM_W5_CLUSTER_NODE_COUNT={node_count}\n", result.stdout
                )

    def test_cli_rejects_unsupported_cluster_size(self):
        result = self.run_config("--print-env", "--nodes", "5")
        self.assertEqual(result.returncode, 2)
        self.assertIn("--nodes must be 2, 3, 4, or 8: 5", result.stderr)

    def test_cli_model_overrides_deepseek_env_source(self):
        model = self.repo / "out" / "test-deepseek-v4-flash.gguf"
        try:
            model.parent.mkdir(parents=True, exist_ok=True)
            model.write_bytes(b"GGUFtest")
            result = self.run_config("--print-env", "--model", str(model))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn(f"SIM_DEEPSEEK_V4_FLASH={model}\n", result.stdout)
        finally:
            model.unlink(missing_ok=True)

    def test_deepseek_env_uses_format_neutral_model_source(self):
        config = self.deepseek_config.read_text(encoding="utf-8")
        self.assertIn("SIM_DEEPSEEK_V4_FLASH=", config)
        self.assertNotIn("SIM_DEEPSEEK_V4_FLASH_GGUF", config)

    def test_launcher_derives_topology_scenario_and_active_nodes(self):
        source = self.launcher.read_text(encoding="utf-8")
        self.assertIn("SIM_W5_CLUSTER_NODE_COUNT", source)
        self.assertIn("ub_topology_two_node_v1_extended.ini", source)
        self.assertIn("ub_topology_three_node_full_mesh.ini", source)
        self.assertIn("ub_topology_four_node_full_mesh.ini", source)
        self.assertIn("ub_topology_eight_node_full_mesh.ini", source)
        self.assertIn("mvp_2host_single_domain.yaml", source)
        self.assertIn("mvp_3host_single_domain.yaml", source)
        self.assertIn("mvp_4host_single_domain.yaml", source)
        self.assertIn("mvp_8host_single_domain.yaml", source)
        self.assertIn("NODE_IDS=(nodeA nodeB)", source)
        self.assertIn("NODE_IDS=(nodeA nodeB nodeC)", source)
        self.assertIn("NODE_IDS=(nodeA nodeB nodeC nodeD)", source)
        self.assertIn(
            'LINQU_UB_NODE_COUNT="$SIM_W5_CLUSTER_NODE_COUNT"', source
        )

    def test_deepseek_guest_validation_uses_active_node_count(self):
        source = self.guest_runner.read_text(encoding="utf-8")
        self.assertIn(
            "pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=43", source
        )
        self.assertNotIn("pipeline_nodes=8 total_layers=43", source)
        self.assertIn(
            "remote_idx=$((idx % SIM_W5_CLUSTER_NODE_COUNT + 1))", source
        )
        self.assertIn(
            "export LINQU_UB_NODE_COUNT=$SIM_W5_CLUSTER_NODE_COUNT", source
        )
        self.assertIn('SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION:-0', source)
        self.assertNotIn('SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION:-1', source)
        self.assertIn(
            "obmm_cluster_runtime_bootstrap local=node${idx} "
            "nodes=$SIM_W5_CLUSTER_NODE_COUNT",
            source,
        )
        infer_source = self.infer_source.read_text(encoding="utf-8")
        runtime_source = self.mem_service_cluster_runtime.read_text(encoding="utf-8")
        self.assertIn(
            "mem_service_obmm_service_v0_pipeline_start_barrier",
            infer_source,
        )
        self.assertIn("pipeline_start_barrier", runtime_source)

    def test_deepseek_runtime_input_uses_model_neutral_range_request(self):
        source = self.infer_source.read_text(encoding="utf-8")
        deepseek_branch = source.split(
            "if (is_deepseek_v4_flash_profile() &&", 1
        )[1].split("if (guest_decode_step > 0)", 1)[0]

        self.assertIn(
            "mem_service_range_flow_wait_runtime_input_view(\n"
            "                           &range_request,",
            deepseek_branch,
        )
        self.assertNotIn(
            "mem_service_obmm_service_v0_wait_runtime_range_input_view(",
            deepseek_branch,
        )

    def test_three_node_assets_describe_one_consistent_topology(self):
        topology = (
            self.repo / "vendor" / "ub_topology_three_node_full_mesh.ini"
        ).read_text(encoding="utf-8")
        scenario = (
            self.repo / "scenarios" / "mvp_3host_single_domain.yaml"
        ).read_text(encoding="utf-8")

        self.assertEqual(topology.count('[node "'), 3)
        self.assertEqual(topology.count('[link "'), 3)
        self.assertIn("hosts: 3", scenario)
        self.assertIn("hosts: [0, 1, 2]", scenario)
        self.assertIn("pe_count: 3", scenario)

    def test_remote_obmm_activation_fails_closed_without_retry_fallback(self):
        source = self.mem_service_cluster_runtime.read_text(encoding="utf-8")
        self.assertIn("if (obmm_do_import", source)
        self.assertIn("activate_remote_failed owner=node%d", source)
        self.assertNotIn("remote_slot_import_retry", source)
        self.assertNotIn("remote_slot_activation_retry", source)
        self.assertIn("mem_service_cluster_runtime_pipeline_start_barrier", source)


if __name__ == "__main__":
    unittest.main()
