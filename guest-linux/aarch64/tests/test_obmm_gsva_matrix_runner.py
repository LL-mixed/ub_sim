import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
RUNNER = ROOT / "scripts" / "run_ub_multi_node_obmm_gsva_matrix.sh"
APP = ROOT / "apps" / "obmm_gsva" / "obmm_gsva.c"


class ObmmGsvaMatrixRunnerTest(unittest.TestCase):
    def print_config(self, nodes: int) -> dict[str, str]:
        result = subprocess.run(
            ["zsh", str(RUNNER), "--nodes", str(nodes), "--print-config"],
            check=True,
            capture_output=True,
            text=True,
        )
        return dict(line.split("=", 1) for line in result.stdout.splitlines())

    def test_resolves_supported_topologies_and_directed_pairs(self):
        expected = {
            2: ("ub_topology_two_node_v1_extended.ini", "2", "nodeA,nodeB"),
            3: ("ub_topology_three_node_full_mesh.ini", "6", "nodeA,nodeB,nodeC"),
            4: (
                "ub_topology_four_node_full_mesh.ini",
                "12",
                "nodeA,nodeB,nodeC,nodeD",
            ),
            8: (
                "ub_topology_eight_node_full_mesh.ini",
                "56",
                "nodeA,nodeB,nodeC,nodeD,nodeE,nodeF,nodeG,nodeH",
            ),
        }
        for nodes, (topology, pairs, names) in expected.items():
            with self.subTest(nodes=nodes):
                config = self.print_config(nodes)
                self.assertEqual(config["node_count"], str(nodes))
                self.assertTrue(config["topology_file"].endswith(topology))
                self.assertEqual(config["directed_pairs"], pairs)
                self.assertEqual(config["nodes"], names)

    def test_rejects_unsupported_node_count_before_launch(self):
        result = subprocess.run(
            ["zsh", str(RUNNER), "--nodes", "5", "--print-config"],
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("node count must be 2, 3, 4, or 8", result.stderr)

    def test_guest_success_record_exposes_verified_pair_count(self):
        source = APP.read_text()
        self.assertIn(
            '"result=done mode=matrix node=%d node_count=%d directed_pairs=%d"',
            source,
        )
        self.assertIn("cfg->node_count * (cfg->node_count - 1)", source)


if __name__ == "__main__":
    unittest.main()
