import json
import pathlib
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[3]
GUEST_ROOT = ROOT / "guest-linux" / "aarch64"
APP_SOURCE = (
    GUEST_ROOT
    / "apps"
    / "lingqu_shmem_pto_direct"
    / "lingqu_shmem_pto_direct.c"
)
RUN_APP = GUEST_ROOT / "initramfs" / "run_app"
LAUNCHER = GUEST_ROOT / "scripts" / "launch_ub_eight_node_headless.sh"
RUNNER = (
    GUEST_ROOT
    / "scripts"
    / "run_ub_eight_node_lingqu_shmem_pto_direct.sh"
)
VALIDATOR = (
    GUEST_ROOT
    / "scripts"
    / "validate_lingqu_shmem_pto_eight_node.py"
)


class LingquShmemPtoEightNodeTest(unittest.TestCase):
    def test_guest_app_assigns_one_lane_to_each_consumer(self):
        source = APP_SOURCE.read_text()

        self.assertIn("#define PTO_DIRECT_MAX_NODES 8u", source)
        self.assertIn("config->node_count > PTO_DIRECT_MAX_NODES", source)
        self.assertIn("config->node_id == 0", source)
        self.assertIn("lane_index * elements + index", source)
        self.assertIn("lane_base(layout, consumer_node_id)", source)
        self.assertIn("config->node_id - 1, lane_base(layout", source)
        self.assertIn("consumers=%u lanes_verified=%u", source)
        self.assertIn("stage=completion_ack_published", source)
        self.assertIn("stage=completion_acks_verified", source)
        self.assertIn("reason=completion_ack_timeout", source)
        self.assertIn("(uint64_t)(config->node_id - 1) << 8", source)

    def test_initramfs_routes_nodes_one_through_seven_to_consumers(self):
        source = RUN_APP.read_text()

        self.assertIn('1|2|3|4|5|6|7) role="consumer"', source)
        self.assertIn("requires node index 0 through 7", source)

    def test_launcher_assigns_node_identity_and_pto_cna(self):
        source = LAUNCHER.read_text()

        self.assertIn("SIM_LINGQU_SHMEM_PTO_ENABLE", source)
        self.assertIn("SIM_LINGQU_SHMEM_PTO_CNA_BASE", source)
        self.assertIn("linqu_node_idx=$node_index", source)
        self.assertIn("linqu_node_count=$SIM_W5_CLUSTER_NODE_COUNT", source)
        self.assertIn("SIM_LINGQU_SHMEM_PTO_CNA_BASE + node_index", source)
        self.assertIn('-global "ubc.pto-device-cna=$pto_device_cna"', source)

    def test_runner_exposes_a_bounded_cli_and_structured_report(self):
        source = RUNNER.read_text()

        self.assertIn("--manifest PATH", source)
        self.assertIn("--cna-base N", source)
        self.assertIn("validation.json", source)
        self.assertIn("validation.status", source)
        self.assertIn("host_dispatch_serialization=possible", source)
        self.assertIn("qemu-leftovers.txt", source)
        self.assertIn("snapshot_simpler_host_artifacts.py", source)
        self.assertIn('echo "source_manifest=$SOURCE_MANIFEST"', source)
        self.assertIn('submodule status --recursive', source)

        result = subprocess.run(
            ["zsh", str(RUNNER), "--help"],
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("Usage:", result.stdout)
        self.assertIn("--manifest PATH", result.stdout)

    def test_runner_stops_qemu_before_snapshotting_buffered_logs(self):
        source = RUNNER.read_text()

        cleanup = source.index('"$CLEANUP_SCRIPT" > "$EVIDENCE_DIR/cleanup.stdout"')
        snapshot = source.index('cp "$source_log" "$EVIDENCE_DIR/logs/')
        self.assertLess(cleanup, snapshot)

    @staticmethod
    def write_valid_logs(logs_dir: pathlib.Path, duplicate_lane: bool = False):
        generation = 101
        fingerprint = 0x1234
        cna_base = 0xF001
        tensor_bytes = 65_536
        lane_stride = tensor_bytes * 3

        (logs_dir / "nodeA_guest.log").write_text(
            "LINGQU_SHMEM_PTO role=producer producer_verify=pass "
            "elements=16384 output_offset=131072 storage_elements=16384 "
            "holes_unchanged=1 elapsed_ms=12 consumers=7\n"
            "LINGQU_SHMEM_PTO role=producer "
            "stage=completion_acks_verified consumers=7 ack_base=1376256\n"
            "LINGQU_SHMEM_PTO_RESULT role=producer status=pass "
            "consumers=7 lanes_verified=7 completion_acks=7\n"
        )
        (logs_dir / "nodeA_qemu.log").write_text("")
        for node_id, node_name in enumerate(
            ("nodeB", "nodeC", "nodeD", "nodeE", "nodeF", "nodeG", "nodeH"),
            start=1,
        ):
            lane_index = node_id - 1
            reported_lane = 5 if duplicate_lane and node_id == 7 else lane_index
            lane_base = reported_lane * lane_stride
            local_pa = 0x1000_0000
            cna = cna_base + node_id
            op_id = (generation << 16) | (lane_index << 8) | 1
            request_id = (generation << 16) | (lane_index << 8) | 2
            ack_offset = 7 * lane_stride + lane_index * 8
            ack_marker = 0x4C51_5054_4F41_434B ^ (generation << 8) ^ node_id
            (logs_dir / f"{node_name}_guest.log").write_text(
                "LINGQU_SHMEM_PTO role=consumer stage=start "
                f"node_id={node_id} node_count=8 generation={generation} "
                f"lane_index={reported_lane} lane_base={lane_base}\n"
                "LINGQU_SHMEM_PTO role=consumer stage=prepared "
                f"local_pa=0x{local_pa:x} requester_cna=0x{cna:x} "
                f"fingerprint=0x{fingerprint:x} node_id={node_id} "
                f"lane_index={reported_lane} lane_base={lane_base} "
                f"op_id={op_id} request_id={request_id}\n"
                "LINGQU_SHMEM_PTO role=consumer "
                "stage=completion_ack_published "
                f"node_id={node_id} ack_offset={ack_offset} "
                f"marker=0x{ack_marker:x}\n"
                "LINGQU_SHMEM_PTO_RESULT role=consumer status=pass "
                f"op_id={op_id} request_id={request_id} node_id={node_id} "
                f"lane_index={reported_lane} elements=16384\n"
            )
            expected_lane_base = lane_index * lane_stride
            (logs_dir / f"{node_name}_qemu.log").write_text(
                f"QEMU_UB_GM_ACCESS_REGISTER pto_device_cna=0x{cna:x}\n"
                "SIM_QEMU_UB_GM_BIND_REGISTER request=x bindings=3 "
                f"requester_cna=0x{cna:x}\n"
                "QEMU_UB_GM_LOAD request=x "
                f"addr=0x{local_pa + expected_lane_base:x} length=65536 ok\n"
                "QEMU_UB_GM_LOAD request=x "
                f"addr=0x{local_pa + expected_lane_base + tensor_bytes:x} "
                "length=65536 ok\n"
                "QEMU_UB_GM_STORE request=x "
                f"addr=0x{local_pa + expected_lane_base + 2 * tensor_bytes:x} "
                "length=65536 ok\n"
                "QEMU_UB_GM_FENCE request=x length=65536\n"
                "QEMU_UB_GM_UNBIND request=x reason=completion_success "
                "bindings=3 load_bytes=131072 store_bytes=65536 fences=1 "
                "segment_payload_staging_bytes=0\n"
            )

    def test_validator_accepts_seven_independent_consumers(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            logs_dir = root / "logs"
            logs_dir.mkdir()
            self.write_valid_logs(logs_dir)
            output = root / "validation.json"

            subprocess.run(
                [
                    "python3",
                    str(VALIDATOR),
                    "--logs-dir",
                    str(logs_dir),
                    "--output",
                    str(output),
                    "--run-id",
                    "test-eight-node",
                    "--artifact-fingerprint",
                    "0x1234",
                    "--cna-base",
                    "0xf001",
                    "--root-commit",
                    "deadbeef",
                ],
                check=True,
            )
            report = json.loads(output.read_text())

        self.assertEqual(report["validation"]["status"], "pass")
        self.assertEqual(report["totals"]["consumer_count"], 7)
        self.assertEqual(report["totals"]["load_calls"], 14)
        self.assertEqual(report["totals"]["store_calls"], 7)
        self.assertEqual(report["totals"]["fences"], 7)
        self.assertEqual(report["totals"]["segment_payload_staging_bytes"], 0)
        self.assertEqual(report["producer"]["completion_acks"], 7)
        self.assertFalse(
            report["execution_boundary"]["host_callable_parallelism_proven"]
        )
        self.assertEqual(
            report["execution_boundary"]["host_dispatch_serialization"],
            "possible",
        )

    def test_validator_rejects_a_duplicate_lane(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            logs_dir = root / "logs"
            logs_dir.mkdir()
            self.write_valid_logs(logs_dir, duplicate_lane=True)
            output = root / "validation.json"

            result = subprocess.run(
                [
                    "python3",
                    str(VALIDATOR),
                    "--logs-dir",
                    str(logs_dir),
                    "--output",
                    str(output),
                    "--run-id",
                    "test-eight-node-invalid",
                    "--artifact-fingerprint",
                    "0x1234",
                    "--cna-base",
                    "0xf001",
                    "--root-commit",
                    "deadbeef",
                ],
                check=False,
            )
            report = json.loads(output.read_text())

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["validation"]["status"], "fail")
        self.assertTrue(
            any("lane" in error for error in report["validation"]["errors"])
        )


if __name__ == "__main__":
    unittest.main()
