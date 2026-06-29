import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
SERVICE_C = SERVICE_DIR / "mem_service_module.c"
SERVICE_H = SERVICE_DIR / "mem_service.h"
SERVICE_CORE_H = SERVICE_DIR / "mem_service_core.h"
SERVICE_QWEN3_H = SERVICE_DIR / "mem_service_qwen3.h"
SERVICE_INTERNAL_H = SERVICE_DIR / "mem_service_internal.h"
SERVICE_CLUSTER_PAYLOAD_CONTRACT_H = (
    SERVICE_DIR / "mem_service_cluster_payload_contract.h"
)
SERVICE_COMPILER_H = SERVICE_DIR / "mem_service_compiler.h"
SERVICE_GUEST_RUNTIME_H = SERVICE_DIR / "mem_service_guest_runtime.h"
SERVICE_OBJECT_CONTRACT_H = SERVICE_DIR / "mem_service_object_contract.h"
SERVICE_QWEN3_PLACEMENT_H = SERVICE_DIR / "mem_service_qwen3_placement.h"
SERVICE_QWEN3_RECORD_POLICY_H = SERVICE_DIR / "mem_service_qwen3_record_policy.h"
SERVICE_RUNTIME_CONFIG_H = SERVICE_DIR / "mem_service_runtime_config.h"
SERVICE_RECORD_TABLE_H = SERVICE_DIR / "mem_service_record_table.h"
SERVICE_QWEN3_RECORDS_H = SERVICE_DIR / "mem_service_qwen3_records.h"
SERVICE_RECORDS_C = SERVICE_DIR / "mem_service_records.c"
SERVICE_KEYS_C = SERVICE_DIR / "mem_service_keys.c"
SERVICE_KEYS_H = SERVICE_DIR / "mem_service_keys.h"
SERVICE_DAEMON_C = SERVICE_DIR / "mem_service_daemon.c"
SERVICE_DAEMON_H = SERVICE_DIR / "mem_service_daemon.h"
SERVICE_WIRE_H = SERVICE_DIR / "mem_service_wire.h"
SERVICE_CLIENT_C = SERVICE_DIR / "mem_service_client.c"
SERVICE_CLIENT_H = SERVICE_DIR / "mem_service_client.h"
SERVICE_WIRE_CLIENT_C = SERVICE_DIR / "mem_service_wire_client.c"
SERVICE_WIRE_CLIENT_H = SERVICE_DIR / "mem_service_wire_client.h"
SERVICE_WIRE_PAYLOAD_H = SERVICE_DIR / "mem_service_wire_payload.h"
SERVICE_WIRE_SCHEMA_H = SERVICE_DIR / "mem_service_wire_schema.h"
SERVICE_DAEMON_RUNTIME_TEST = ROOT / "tests" / "test_mem_service_daemon_runtime.py"
SERVICE_OBJECT_REFS_C = SERVICE_DIR / "mem_service_object_refs.c"
SERVICE_OBJECT_REFS_H = SERVICE_DIR / "mem_service_object_refs.h"
SERVICE_OBMM_OBJECTS_C = SERVICE_DIR / "mem_service_obmm_objects.c"
SERVICE_OBMM_OBJECTS_H = SERVICE_DIR / "mem_service_obmm_objects.h"
SERVICE_QWEN3_RECORDS_C = SERVICE_DIR / "mem_service_qwen3_records.c"
SERVICE_QWEN3_RUNTIME_C = SERVICE_DIR / "mem_service_qwen3_runtime.c"
SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_C = (
    SERVICE_DIR / "mem_service_qwen3_runtime_range_wait_flow.c"
)
SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_C = (
    SERVICE_DIR / "mem_service_qwen3_runtime_range_publish_flow.c"
)
SERVICE_QWEN3_KV_STATE_FLOW_C = SERVICE_DIR / "mem_service_qwen3_kv_state_flow.c"
SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_C = (
    SERVICE_DIR / "mem_service_qwen3_terminal_token_flow.c"
)
SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_C = (
    SERVICE_DIR / "mem_service_qwen3_engram_publish_flow.c"
)
SERVICE_QWEN3_ENGRAM_WAIT_FLOW_C = SERVICE_DIR / "mem_service_qwen3_engram_wait_flow.c"
SERVICE_QWEN3_DECODE_BARRIER_C = SERVICE_DIR / "mem_service_qwen3_decode_barrier.c"
SERVICE_METADATA_C = SERVICE_DIR / "mem_service_metadata.c"
SERVICE_CLUSTER_PAYLOAD_C = SERVICE_DIR / "mem_service_cluster_payload.c"
SERVICE_CLUSTER_PAYLOAD_H = SERVICE_DIR / "mem_service_cluster_payload.h"
SERVICE_CLUSTER_READ_C = SERVICE_DIR / "mem_service_cluster_read.c"
SERVICE_CLUSTER_READ_H = SERVICE_DIR / "mem_service_cluster_read.h"
SERVICE_CLUSTER_UTILS_C = SERVICE_DIR / "mem_service_cluster_utils.c"
SERVICE_CLUSTER_UTILS_H = SERVICE_DIR / "mem_service_cluster_utils.h"
SERVICE_CLUSTER_RUNTIME_C = SERVICE_DIR / "mem_service_cluster_runtime.c"
SERVICE_CLUSTER_RUNTIME_H = SERVICE_DIR / "mem_service_cluster_runtime.h"
SERVICE_CLUSTER_QUEUE_C = SERVICE_DIR / "mem_service_cluster_queue.c"
SERVICE_CLUSTER_QUEUE_H = SERVICE_DIR / "mem_service_cluster_queue.h"
SERVICE_CLUSTER_OBSERVE_C = SERVICE_DIR / "mem_service_cluster_observe.c"
SERVICE_CLUSTER_OBSERVE_H = SERVICE_DIR / "mem_service_cluster_observe.h"
SERVICE_OBMM_OBJECT_FLOW_C = SERVICE_DIR / "mem_service_obmm_object_flow.c"
SERVICE_OBMM_OBJECT_FLOW_H = SERVICE_DIR / "mem_service_obmm_object_flow.h"
GUEST_C = ROOT / "apps" / "llm_infer" / "llm_infer.c"
BUILD_INITRAMFS = ROOT / "scripts" / "build_initramfs.sh"
RUN_APP = ROOT / "initramfs" / "run_app"
COMPONENTS_README = ROOT / "components" / "README.md"
CLI_DIR = ROOT / "apps" / "mem_service"
FOUR_NODE_W4_RUNNER = ROOT / "scripts" / "run_ub_four_node_w4_guest.sh"
EIGHT_NODE_W4_RUNNER = ROOT / "scripts" / "run_ub_eight_node_w4_guest.sh"
SIM_UAPI_RS = REPO_ROOT / "crates" / "sim-uapi" / "src" / "lib.rs"


class MemServiceRecordRecyclingTests(unittest.TestCase):
    def test_mem_service_has_cli_without_demo_naming(self):
        build_script = BUILD_INITRAMFS.read_text()
        run_app = RUN_APP.read_text()
        components_readme = COMPONENTS_README.read_text()
        cli_source = (CLI_DIR / "mem_service.c").read_text()
        cli_makefile = (CLI_DIR / "Makefile").read_text()
        release_manifest = (CLI_DIR / "release-manifest.txt").read_text()
        package_manifest = (CLI_DIR / "package-manifest.txt").read_text()
        admin_output_schema = (CLI_DIR / "admin-output-schema.txt").read_text()
        upgrade_rollback_policy = (CLI_DIR / "upgrade-rollback-policy.txt").read_text()
        ops_certification_policy = (
            CLI_DIR / "ops-certification-policy.txt"
        ).read_text()
        alert_rules = (
            CLI_DIR / "deploy" / "linqu_mem_service.prometheus-alerts.yml"
        ).read_text()
        api_abi_policy = (CLI_DIR / "api-abi-policy.txt").read_text()
        config_schema = (CLI_DIR / "configs" / "mem_service.conf.schema").read_text()
        config_example = (CLI_DIR / "configs" / "mem_service.example.conf").read_text()
        config_runtime = (CLI_DIR / "configs" / "mem_service.runtime.conf").read_text()
        config_host_runtime = (
            CLI_DIR / "configs" / "mem_service.host.runtime.conf"
        ).read_text()
        deploy_manifest = (CLI_DIR / "deploy" / "linqu_mem_service.service").read_text()
        host_deploy_manifest = (
            CLI_DIR / "deploy" / "linqu_mem_service.host.service"
        ).read_text()
        serving_example = (
            CLI_DIR / "examples" / "mem_service_serving_example.c"
        ).read_text()
        pretraining_example = (
            CLI_DIR / "examples" / "mem_service_pretraining_example.c"
        ).read_text()
        client_header = SERVICE_CLIENT_H.read_text()

        self.assertIn("Components do not install guest binaries directly", components_readme)
        self.assertIn(
            'MEM_SERVICE_SRC="$ROOT_DIR/components/mem_service/mem_service_module.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_UTILS_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_utils.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_PAYLOAD_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_payload.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_READ_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_read.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_RUNTIME_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_runtime.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_QUEUE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_queue.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLUSTER_OBSERVE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_observe.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_OBMM_OBJECT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_object_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_METADATA_SRC="$ROOT_DIR/components/mem_service/mem_service_metadata.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_DAEMON_SRC="$ROOT_DIR/components/mem_service/mem_service_daemon.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLIENT_SRC="$ROOT_DIR/components/mem_service/mem_service_client.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_WIRE_CLIENT_SRC="$ROOT_DIR/components/mem_service/mem_service_wire_client.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_KEYS_SRC="$ROOT_DIR/components/mem_service/mem_service_keys.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_OBJECT_REFS_SRC="$ROOT_DIR/components/mem_service/mem_service_object_refs.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_OBMM_OBJECTS_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_objects.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_records.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_records.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_RUNTIME_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_decode_barrier.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_kv_state_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_terminal_token_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_engram_publish_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_engram_wait_flow.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_QWEN3_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3.c"',
            build_script,
        )
        self.assertIn(
            'MEM_SERVICE_CLI_SRC="$ROOT_DIR/apps/mem_service/mem_service.c"',
            build_script,
        )
        self.assertIn('MEM_SERVICE_CLI_BIN="$OUT_DIR/linqu_mem_service"', build_script)
        self.assertIn(
            'MEM_SERVICE_QWEN3_CLI_BIN="$OUT_DIR/linqu_mem_service_qwen3"',
            build_script,
        )
        self.assertIn(
            '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_DAEMON_SRC" '
            '"$MEM_SERVICE_CLIENT_SRC" '
            '"$MEM_SERVICE_WIRE_CLIENT_SRC" '
            '"$MEM_SERVICE_METADATA_SRC" '
            '"$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" '
            '"$MEM_SERVICE_RECORDS_SRC" -lm -o "$MEM_SERVICE_CLI_BIN"',
            build_script,
        )
        self.assertIn(
            "-DMEM_SERVICE_ENABLE_QWEN3_INSPECT",
            build_script,
        )
        self.assertIn(
            '"$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" '
            '"$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" '
            '"$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" '
            '"$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" '
            '"$MEM_SERVICE_DAEMON_SRC" "$MEM_SERVICE_CLIENT_SRC" '
            '"$MEM_SERVICE_WIRE_CLIENT_SRC" '
            '"$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" '
            '"$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" '
            '"$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" '
            '"$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" '
            '"$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" '
            '"$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" '
            '"$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" '
            '"$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" '
            '"$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" '
            '"$LLM_INFER_SRC" -lm -o "$MEM_SERVICE_QWEN3_CLI_BIN"',
            build_script,
        )
        self.assertIn(
            '"$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"',
            build_script,
        )
        self.assertIn("linqu_mem_service", build_script)
        self.assertIn("linqu_mem_service", run_app)
        self.assertIn("linqu_mem_service=1", run_app)
        self.assertIn("run_binary \"linqu_mem_service\" /bin/linqu_mem_service --smoke", run_app)
        self.assertIn(
            'run_binary "linqu_mem_service_wire_fixtures" '
            "/bin/linqu_mem_service wire-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_wire_schema_fixtures" '
            "/bin/linqu_mem_service wire-schema-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_store_fixtures" '
            "/bin/linqu_mem_service store-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_journal_fixtures" '
            "/bin/linqu_mem_service journal-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_config_fixtures" '
            "/bin/linqu_mem_service config-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_metrics_export_fixtures" '
            "/bin/linqu_mem_service metrics-export-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_collector_fixtures" '
            "/bin/linqu_mem_service collector-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_deployment_fixtures" '
            "/bin/linqu_mem_service deployment-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_admin_output_fixtures" '
            "/bin/linqu_mem_service admin-output-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_upgrade_rollback_fixtures" '
            "/bin/linqu_mem_service upgrade-rollback-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_alert_fixtures" '
            "/bin/linqu_mem_service alert-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_alert_integration_fixtures" '
            "/bin/linqu_mem_service alert-integration-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_ops_certification_fixtures" '
            "/bin/linqu_mem_service ops-certification-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_ops_certification_evidence_fixtures" '
            "/bin/linqu_mem_service ops-certification-evidence-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_client_retry_fixtures" '
            "/bin/linqu_mem_service client-retry-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_api_abi_fixtures" '
            "/bin/linqu_mem_service api-abi-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_compat_fixtures" '
            "/bin/linqu_mem_service compat-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_compat_baseline_fixtures" '
            "/bin/linqu_mem_service compat-baseline-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_compat_old_new_fixtures" '
            "/bin/linqu_mem_service compat-old-new-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_package_fixtures" '
            "/bin/linqu_mem_service package-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_release_fixtures" '
            "/bin/linqu_mem_service release-fixtures",
            run_app,
        )
        self.assertIn(
            'run_binary "linqu_mem_service_qwen3" /bin/linqu_mem_service_qwen3 --inspect-qwen3',
            run_app,
        )
        self.assertTrue((CLI_DIR / "mem_service.c").exists())
        self.assertTrue((CLI_DIR / "Makefile").exists())
        self.assertIn('#include "components/mem_service/mem_service_core.h"', cli_source)
        self.assertIn('#include "components/mem_service/mem_service_daemon.h"', cli_source)
        self.assertIn('#include "components/mem_service/mem_service_wire_client.h"', cli_source)
        self.assertIn('strcmp(argv[1], "wire-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "wire-schema")', cli_source)
        self.assertIn('strcmp(argv[1], "wire-schema-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "journal-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "journal-compaction-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "journal-torn-recovery-fixtures")', cli_source)
        self.assertIn("mem_service_run_journal_compaction_fixture_check", cli_source)
        self.assertIn(
            "mem_service_run_journal_torn_recovery_fixture_check", cli_source
        )
        self.assertIn('strcmp(argv[1], "store-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "config-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "metrics-export-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "collector-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "admin-output-schema")', cli_source)
        self.assertIn('strcmp(argv[1], "admin-output-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "upgrade-rollback-policy")', cli_source)
        self.assertIn('strcmp(argv[1], "upgrade-rollback-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "alert-rules")', cli_source)
        self.assertIn('strcmp(argv[1], "alert-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "alert-integration-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-policy")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-evidence-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-generate-evidence")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-linux-ci-smoke")', cli_source)
        self.assertIn('strcmp(argv[1], "ops-certification-verify")', cli_source)
        self.assertIn('strcmp(argv[1], "client-retry-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "api-abi-policy")', cli_source)
        self.assertIn('strcmp(argv[1], "api-abi-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-matrix")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-baseline-v1")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-baseline-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-old-new-matrix")', cli_source)
        self.assertIn('strcmp(argv[1], "compat-old-new-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "package-manifest")', cli_source)
        self.assertIn('strcmp(argv[1], "package-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "release-manifest")', cli_source)
        self.assertIn('strcmp(argv[1], "release-fixtures")', cli_source)
        self.assertIn("run_wire_schema_manifest", cli_source)
        self.assertIn("run_wire_schema_fixture_check", cli_source)
        self.assertIn("render_metrics_prometheus_text", cli_source)
        self.assertIn("run_metrics_export_fixture_check", cli_source)
        self.assertIn("run_collector_fixture_check", cli_source)
        self.assertIn("collector_metric_value_at_least", cli_source)
        self.assertIn("run_client_retry_fixture_check", cli_source)
        self.assertIn("render_api_abi_policy", cli_source)
        self.assertIn("run_api_abi_fixture_check", cli_source)
        self.assertIn("render_admin_output_schema", cli_source)
        self.assertIn("run_admin_output_fixture_check", cli_source)
        self.assertIn("render_upgrade_rollback_policy", cli_source)
        self.assertIn("run_upgrade_rollback_fixture_check", cli_source)
        self.assertIn("render_alert_rules", cli_source)
        self.assertIn("run_alert_fixture_check", cli_source)
        self.assertIn("run_compat_matrix", cli_source)
        self.assertIn("run_compat_fixture_check", cli_source)
        self.assertIn("run_compat_baseline_v1", cli_source)
        self.assertIn("run_compat_baseline_fixture_check", cli_source)
        self.assertIn("run_compat_old_new_matrix", cli_source)
        self.assertIn("run_compat_old_new_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "compat-runtime-fixtures")', cli_source)
        self.assertIn("mem_service_run_compat_runtime_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "compat-old-server-runtime-fixtures")', cli_source)
        self.assertIn("mem_service_run_compat_old_server_runtime_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "serving-fail-closed-fixtures")', cli_source)
        self.assertIn("mem_service_run_serving_fail_closed_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "pretraining-fail-closed-fixtures")', cli_source)
        self.assertIn("mem_service_run_pretraining_fail_closed_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "typed-payload-fixtures")', cli_source)
        self.assertIn("mem_service_run_typed_payload_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "deployment-fixtures")', cli_source)
        self.assertIn("MEM_SERVICE_DEPLOYMENT_SMOKE_VERSION 1U", cli_source)
        self.assertIn("render_metrics_http_response", cli_source)
        self.assertIn("run_deployment_fixture_check", cli_source)
        self.assertIn("MEM_SERVICE_COMPAT_MATRIX_EXPECTED_LEN 1978U", cli_source)
        self.assertIn(
            "MEM_SERVICE_COMPAT_MATRIX_EXPECTED_CHECKSUM 0x1844c64dU",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_LEN 1251U", cli_source)
        self.assertIn(
            "MEM_SERVICE_COMPAT_BASELINE_V1_EXPECTED_CHECKSUM 0xdac5b8d5U",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_LEN 1733U", cli_source)
        self.assertIn(
            "MEM_SERVICE_COMPAT_OLD_NEW_MATRIX_EXPECTED_CHECKSUM 0x6509c49dU",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_LEN 9220U", cli_source)
        self.assertIn(
            "MEM_SERVICE_WIRE_SCHEMA_MANIFEST_EXPECTED_CHECKSUM 0xce883650U",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_LEN 6624U", cli_source)
        self.assertIn(
            "MEM_SERVICE_ADMIN_OUTPUT_SCHEMA_EXPECTED_CHECKSUM 0x7021f4cfU",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_LEN 2019U", cli_source)
        self.assertIn(
            "MEM_SERVICE_UPGRADE_ROLLBACK_POLICY_EXPECTED_CHECKSUM 0xcdcd3550U",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_LEN 1118U", cli_source)
        self.assertIn(
            "MEM_SERVICE_OPS_CERTIFICATION_POLICY_EXPECTED_CHECKSUM 0xe77c644bU",
            cli_source,
        )
        self.assertIn("MEM_SERVICE_OPS_CERTIFICATION_EVIDENCE_VERSION 1U", cli_source)
        self.assertIn("MEM_SERVICE_REMOTE_TRANSPORT_EVIDENCE_VERSION 1U", cli_source)
        self.assertIn("run_remote_transport_evidence_fixture_check", cli_source)
        self.assertIn("run_remote_transport_generate_evidence", cli_source)
        self.assertIn("run_remote_transport_verify", cli_source)
        self.assertIn("MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_LEN 4944U", cli_source)
        self.assertIn(
            "MEM_SERVICE_PACKAGE_MANIFEST_EXPECTED_CHECKSUM 0x1e9f6129U",
            cli_source,
        )
        self.assertIn(
            'MEM_SERVICE_PACKAGE_TARBALL_NAME "linqu_mem_service-installed-layout-v1.tar"',
            cli_source,
        )
        self.assertIn(
            'MEM_SERVICE_NATIVE_DEB_NAME "linqu-mem-service_0.1.0-1_arm64.deb"',
            cli_source,
        )
        self.assertIn(
            'MEM_SERVICE_NATIVE_RPM_NAME "linqu-mem-service-0.1.0-1.aarch64.rpm"',
            cli_source,
        )
        self.assertIn("MEM_SERVICE_API_ABI_POLICY_EXPECTED_LEN 856U", cli_source)
        self.assertIn(
            "MEM_SERVICE_API_ABI_POLICY_EXPECTED_CHECKSUM 0x5d95ae02U",
            cli_source,
        )
        self.assertIn("run_release_manifest", cli_source)
        self.assertIn("run_release_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "serve")', cli_source)
        self.assertIn('option_value(argc, argv, "--config")', cli_source)
        self.assertIn('option_value(argc, argv, "--store")', cli_source)
        self.assertIn("load_mem_service_config", cli_source)
        self.assertIn("MEM_SERVICE_CONFIG_SCHEMA_VERSION 1U", cli_source)
        self.assertIn("mem_service_run_unix_daemon_with_store_metrics_and_catalog", cli_source)
        self.assertIn('strcmp(argv[1], "durable-catalog-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "chunked-block-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "transport-block-fixtures")', cli_source)
        self.assertIn('strcmp(argv[1], "network-transport-block-fixtures")', cli_source)
        self.assertIn("mem_service_run_chunked_block_fixture_check", cli_source)
        self.assertIn("mem_service_run_transport_block_fixture_check", cli_source)
        self.assertIn("mem_service_run_network_transport_block_fixture_check", cli_source)
        self.assertIn('strcmp(argv[1], "health")', cli_source)
        self.assertIn('strcmp(argv[1], "ready")', cli_source)
        self.assertIn('strcmp(argv[1], "status")', cli_source)
        self.assertIn('strcmp(argv[1], "list-records")', cli_source)
        self.assertIn('strcmp(argv[1], "metrics")', cli_source)
        self.assertIn('strcmp(argv[1], "audit-log")', cli_source)
        self.assertIn('strcmp(argv[1], "metrics-export")', cli_source)
        self.assertIn("lingqu_mem_service_request_count", cli_source)
        self.assertIn("--max-attempts", cli_source)
        self.assertIn("--retry-backoff-ms", cli_source)
        self.assertIn("--retry-timeouts", cli_source)
        self.assertIn('strcmp(argv[1], "export-snapshot")', cli_source)
        self.assertIn('strcmp(argv[1], "export-snapshot-page")', cli_source)
        self.assertIn('strcmp(argv[1], "export-snapshot-to")', cli_source)
        self.assertIn('strcmp(argv[1], "restore-snapshot")', cli_source)
        self.assertIn('strcmp(argv[1], "put-object")', cli_source)
        self.assertIn('strcmp(argv[1], "get-object")', cli_source)
        self.assertIn('strcmp(argv[1], "inspect-object")', cli_source)
        self.assertIn('strcmp(argv[1], "register-prefix")', cli_source)
        self.assertIn('strcmp(argv[1], "lookup-prefix")', cli_source)
        self.assertIn('strcmp(argv[1], "publish-kv")', cli_source)
        self.assertIn('strcmp(argv[1], "resolve-kv")', cli_source)
        self.assertIn('strcmp(argv[1], "publish-runtime-handoff")', cli_source)
        self.assertIn('strcmp(argv[1], "resolve-runtime-handoff")', cli_source)
        self.assertIn('strcmp(argv[1], "register-execution-artifact")', cli_source)
        self.assertIn('strcmp(argv[1], "query-execution-artifact")', cli_source)
        self.assertIn('strcmp(argv[1], "register-training-artifact")', cli_source)
        self.assertIn('strcmp(argv[1], "query-training-artifact")', cli_source)
        self.assertIn('strcmp(argv[1], "commit-training-step")', cli_source)
        self.assertIn('strcmp(argv[1], "resolve-training-step")', cli_source)
        self.assertIn("#ifdef MEM_SERVICE_ENABLE_QWEN3_INSPECT", cli_source)
        self.assertIn('#include "components/llm_infer/llm_infer.h"', cli_source)
        self.assertIn("linqu_mem_service_core", cli_makefile)
        self.assertIn("linqu_mem_service_qwen3", cli_makefile)
        self.assertIn("MEM_SERVICE_RELEASE_MANIFEST := release-manifest.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_PACKAGE_MANIFEST := package-manifest.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_WIRE_SCHEMA_MANIFEST := wire-schema.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_ADMIN_OUTPUT_SCHEMA := admin-output-schema.txt", cli_makefile)
        self.assertIn(
            "MEM_SERVICE_UPGRADE_ROLLBACK_POLICY := upgrade-rollback-policy.txt",
            cli_makefile,
        )
        self.assertIn(
            "MEM_SERVICE_OPS_CERTIFICATION_POLICY := ops-certification-policy.txt",
            cli_makefile,
        )
        self.assertIn("MEM_SERVICE_API_ABI_POLICY := api-abi-policy.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_COMPAT_MATRIX := compat-matrix.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_COMPAT_BASELINE_V1 := compat-baseline-v1.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_COMPAT_OLD_NEW_MATRIX := compat-old-new-matrix.txt", cli_makefile)
        self.assertIn("MEM_SERVICE_CONFIG_SCHEMA := configs/mem_service.conf.schema", cli_makefile)
        self.assertIn("MEM_SERVICE_CONFIG_EXAMPLE := configs/mem_service.example.conf", cli_makefile)
        self.assertIn("MEM_SERVICE_CONFIG_RUNTIME := configs/mem_service.runtime.conf", cli_makefile)
        self.assertIn(
            "MEM_SERVICE_CONFIG_HOST_RUNTIME := configs/mem_service.host.runtime.conf",
            cli_makefile,
        )
        self.assertIn("MEM_SERVICE_DEPLOY_MANIFEST := deploy/linqu_mem_service.service", cli_makefile)
        self.assertIn("MEM_SERVICE_HOST_DEPLOY_MANIFEST := deploy/linqu_mem_service.host.service", cli_makefile)
        self.assertIn(
            "MEM_SERVICE_ALERT_RULES := deploy/linqu_mem_service.prometheus-alerts.yml",
            cli_makefile,
        )
        self.assertIn(
            "MEM_SERVICE_PACKAGE_TARBALL_NAME := linqu_mem_service-installed-layout-v1.tar",
            cli_makefile,
        )
        self.assertIn("MEM_SERVICE_DEB_NAME := linqu-mem-service", cli_makefile)
        self.assertIn("MEM_SERVICE_DEB_ARCH ?= arm64", cli_makefile)
        self.assertIn("package-tarball:", cli_makefile)
        self.assertIn("package-tarball-smoke: package-tarball", cli_makefile)
        self.assertIn("package-deb:", cli_makefile)
        self.assertIn("package-deb-smoke: package-deb", cli_makefile)
        self.assertIn("package-rpm:", cli_makefile)
        self.assertIn("package-rpm-smoke: package-rpm", cli_makefile)
        self.assertIn(
            "linux-ops-certification-smoke: package-rpm-smoke linqu_mem_service_host",
            cli_makefile,
        )
        self.assertIn("OPS_CERTIFICATION_ROLLBACK_RPM ?=", cli_makefile)
        self.assertIn(
            "linux-ops-upgrade-rollback-smoke: package-rpm-smoke",
            cli_makefile,
        )
        self.assertIn(
            "linux-ops-deployment-smoke: linux-ops-upgrade-rollback-smoke linqu_mem_service_host",
            cli_makefile,
        )
        self.assertIn("install: $(MEM_SERVICE_RELEASE_MANIFEST)", cli_makefile)
        self.assertIn("rm -f linqu_mem_service linqu_mem_service_host", cli_makefile)
        self.assertIn(
            '$(MAKE) -B linqu_mem_service CC="$(CC)" CFLAGS="$(CFLAGS)"',
            cli_makefile,
        )
        self.assertIn(
            '$(MAKE) -B linqu_mem_service_host HOST_CC="$(HOST_CC)" '
            'HOST_CFLAGS="$(HOST_CFLAGS)"',
            cli_makefile,
        )
        self.assertIn("tar -cf $(PACKAGE_TARBALL) -C $(PACKAGE_STAGE_ROOT) usr", cli_makefile)
        self.assertIn(
            'f.write(b"!<arch>\\n")',
            cli_makefile,
        )
        self.assertIn("MEM_SERVICE_CLIENT_EXAMPLES :=", cli_makefile)
        self.assertIn("examples/mem_service_serving_example.c", cli_makefile)
        self.assertIn("examples/mem_service_pretraining_example.c", cli_makefile)
        self.assertIn("INSTALL_EXAMPLEDIR := $(INSTALL_DATADIR)/examples", cli_makefile)
        self.assertIn("INSTALL_CONFIGDIR := $(INSTALL_DATADIR)/config", cli_makefile)
        self.assertIn("INSTALL_DEPLOYDIR := $(INSTALL_DATADIR)/deploy", cli_makefile)
        self.assertIn("INSTALL_HOSTDIR := $(DESTDIR)$(PREFIX)/libexec/lingqu/mem_service", cli_makefile)
        self.assertIn("SYSCONFDIR ?= /etc", cli_makefile)
        self.assertIn("INSTALL_SYSCONFDIR := $(DESTDIR)$(SYSCONFDIR)/lingqu/mem_service", cli_makefile)
        self.assertIn("SYSTEMDUNITDIR ?= /usr/lib/systemd/system", cli_makefile)
        self.assertIn("INSTALL_SYSTEMDUNITDIR := $(DESTDIR)$(SYSTEMDUNITDIR)", cli_makefile)
        self.assertIn("linqu_mem_service_host: $(MEM_SERVICE_CORE_SRCS)", cli_makefile)
        self.assertIn("host-artifact-smoke: linqu_mem_service_host", cli_makefile)
        self.assertIn("./linqu_mem_service_host upgrade-rollback-runtime-fixtures", cli_makefile)
        self.assertIn("./linqu_mem_service_host compat-runtime-fixtures", cli_makefile)
        self.assertIn("./linqu_mem_service_host compat-old-server-runtime-fixtures", cli_makefile)
        self.assertIn("./linqu_mem_service_host serving-fail-closed-fixtures", cli_makefile)
        self.assertIn("./linqu_mem_service_host pretraining-fail-closed-fixtures", cli_makefile)
        self.assertIn("./linqu_mem_service_host typed-payload-fixtures", cli_makefile)
        self.assertIn("MEM_SERVICE_PUBLIC_HEADERS :=", cli_makefile)
        self.assertIn("MEM_SERVICE_CLIENT_SDK_SRCS :=", cli_makefile)
        self.assertIn("$(MEM_SERVICE_CONFIG_SCHEMA)", cli_makefile)
        self.assertIn("$(MEM_SERVICE_CONFIG_EXAMPLE)", cli_makefile)
        self.assertIn("$(MEM_SERVICE_DEPLOY_MANIFEST)", cli_makefile)
        self.assertIn("$(MEM_SERVICE_HOST_DEPLOY_MANIFEST)", cli_makefile)
        self.assertIn("^metrics_export_format=prometheus-text$$", cli_makefile)
        self.assertIn("^admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt$$", cli_makefile)
        self.assertIn("^admin_output_schema_checksum=0x7021f4cf$$", cli_makefile)
        self.assertIn("^admin_output_format=text-kv$$", cli_makefile)
        self.assertIn("^admin_metric_prefix=lingqu_mem_service_$$", cli_makefile)
        self.assertIn(
            "^upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt$$",
            cli_makefile,
        )
        self.assertIn("^package_manifest_checksum=0x1e9f6129$$", cli_makefile)
        self.assertIn("installed-sdk-example-smoke: install", cli_makefile)
        self.assertIn("$(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c", cli_makefile)
        self.assertIn("$(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c", cli_makefile)
        self.assertIn("$(INSTALL_SRCDIR)/mem_service_client.c", cli_makefile)
        self.assertIn("$(INSTALL_SRCDIR)/mem_service_wire_client.c", cli_makefile)
        self.assertIn("^installed_sdk_example_smoke=installed-sdk-example-smoke$$", cli_makefile)
        self.assertIn("^package_gate=package-fixtures$$", cli_makefile)
        self.assertIn("^distributable_package_format=tar$$", cli_makefile)
        self.assertIn("^distributable_package_gate=package-tarball-smoke$$", cli_makefile)
        self.assertIn("^native_package_format=deb$$", cli_makefile)
        self.assertIn("^native_package_gate=package-deb-smoke$$", cli_makefile)
        self.assertIn("^rpm_native_package_format=rpm$$", cli_makefile)
        self.assertIn("^rpm_native_package_gate=package-rpm-smoke$$", cli_makefile)
        self.assertIn("^upgrade_rollback_policy_checksum=0xcdcd3550$$", cli_makefile)
        self.assertIn(
            "^upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures$$",
            cli_makefile,
        )
        self.assertIn("^compat_runtime_gate=compat-runtime-fixtures$$", cli_makefile)
        self.assertIn("^serving_fail_closed_matrix=certified$$", cli_makefile)
        self.assertIn("^pretraining_fail_closed_matrix=certified$$", cli_makefile)
        self.assertIn("^wire_payload_typed_binary_format=typed-binary-v1$$", cli_makefile)
        self.assertIn("^upgrade_policy=current-version-only$$", cli_makefile)
        self.assertIn("^rollback_policy=current-version-only$$", cli_makefile)
        self.assertIn("^old_server_runtime_binary=certified$$", cli_makefile)
        self.assertIn("^client_retry_policy=explicit-max-attempts-backoff$$", cli_makefile)
        self.assertIn("^api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt$$", cli_makefile)
        self.assertIn("^api_abi_policy_checksum=0x5d95ae02$$", cli_makefile)
        self.assertIn("^client_api_version=1$$", cli_makefile)
        self.assertIn("^client_abi_version=1$$", cli_makefile)
        self.assertIn("^client_record_abi_size=744$$", cli_makefile)
        self.assertIn("^compat_matrix=share/lingqu/mem_service/compat-matrix.txt$$", cli_makefile)
        self.assertIn("^compat_matrix_checksum=0x1844c64d$$", cli_makefile)
        self.assertIn("^compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt$$", cli_makefile)
        self.assertIn("^compat_baseline_checksum=0xdac5b8d5$$", cli_makefile)
        self.assertIn("^compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt$$", cli_makefile)
        self.assertIn("^compat_old_new_matrix_checksum=0x6509c49d$$", cli_makefile)
        self.assertIn("^host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host$$", cli_makefile)
        self.assertIn("^host_daemon_artifact_smoke=host-artifact-smoke$$", cli_makefile)
        self.assertIn("^host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service$$", cli_makefile)
        self.assertIn("^deployment_smoke=deployment-fixtures$$", cli_makefile)
        self.assertIn("^host_service_manager_smoke=installed-host-service-manager-smoke$$", cli_makefile)
        self.assertIn("^host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm$$", cli_makefile)
        self.assertIn("^collector_smoke=collector-fixtures$$", cli_makefile)
        self.assertIn("^collector_integration_smoke=installed-host-collector-smoke$$", cli_makefile)
        self.assertIn("^collector_scrape_contract=prometheus-text-http-v0.0.4$$", cli_makefile)
        self.assertIn(
            "^alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml$$",
            cli_makefile,
        )
        self.assertIn("^alert_rules_checksum=0xbdff2246$$", cli_makefile)
        self.assertIn("^alert_rule_count=5$$", cli_makefile)
        self.assertIn("^alert_integration_smoke=alert-integration-fixtures$$", cli_makefile)
        self.assertIn(
            "^ops_certification_policy=share/lingqu/mem_service/ops-certification-policy.txt$$",
            cli_makefile,
        )
        self.assertIn("^ops_certification_gate=ops-certification-fixtures$$", cli_makefile)
        self.assertIn(
            "^ops_certification_evidence_schema=ops-certification-evidence-v1$$",
            cli_makefile,
        )
        self.assertIn(
            "^ops_certification_evidence_gate=ops-certification-evidence-fixtures$$",
            cli_makefile,
        )
        self.assertIn(
            "^ops_certification_generate=ops-certification-generate-evidence$$",
            cli_makefile,
        )
        self.assertIn(
            "^ops_certification_linux_ci_gate=ops-certification-linux-ci-smoke$$",
            cli_makefile,
        )
        self.assertIn(
            "^linux_ops_certification_smoke=linux-ops-certification-smoke$$",
            cli_makefile,
        )
        self.assertIn(
            "^linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke$$",
            cli_makefile,
        )
        self.assertIn(
            "^linux_ops_deployment_smoke=linux-ops-deployment-smoke$$",
            cli_makefile,
        )
        self.assertIn(
            "^ops_certification_verify=ops-certification-verify --evidence-file$$",
            cli_makefile,
        )
        self.assertIn("^real_systemd_environment=not-certified$$", cli_makefile)
        self.assertIn(
            "^production_collector_alert_environment=not-certified$$",
            cli_makefile,
        )
        self.assertIn("^rpm_package=not-certified$$", cli_makefile)
        self.assertIn(
            "^service_manager_lifecycle=serve-config-ready-scrape-sigterm$$",
            cli_makefile,
        )
        self.assertIn("^service_manager_shutdown=signal-clean-stop$$", cli_makefile)
        self.assertIn("^durable_backend=snapshot+journal$$", cli_makefile)
        self.assertIn("^durable_catalog=storage-root-v1$$", cli_makefile)
        self.assertIn("^durable_catalog_manifest=catalog/manifest.txt$$", cli_makefile)
        self.assertIn("^payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1$$", cli_makefile)
        self.assertIn("^remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1$$", cli_makefile)
        self.assertIn("^remote_payload_block_data_gate=transport-block-fixtures$$", cli_makefile)
        self.assertIn("^remote_payload_network_transport=tcp-loopback-certified$$", cli_makefile)
        self.assertIn("^remote_payload_network_transport_gate=network-transport-block-fixtures$$", cli_makefile)
        self.assertIn("^remote_payload_network_transport_make_gate=network-transport-block-smoke$$", cli_makefile)
        self.assertIn("^remote_payload_production_network_transport=not-certified$$", cli_makefile)
        self.assertIn("^remote_payload_production_transport_evidence_schema=remote-transport-evidence-v1$$", cli_makefile)
        self.assertIn("^remote_payload_production_transport_evidence_gate=remote-transport-evidence-fixtures$$", cli_makefile)
        self.assertIn("^remote_payload_production_transport_generate=remote-transport-generate-evidence$$", cli_makefile)
        self.assertIn("^remote_payload_production_transport_verify=remote-transport-verify --evidence-file$$", cli_makefile)
        self.assertIn("^remote_payload_production_transport_ci=scripts/run_mem_service_remote_transport_ci.sh$$", cli_makefile)
        self.assertIn("^required_gate=remote-transport-evidence-fixtures$$", cli_makefile)
        self.assertIn("network-transport-block-smoke: linqu_mem_service_host", cli_makefile)
        self.assertIn("^metrics_listen_config=metrics_listen$$", cli_makefile)
        self.assertIn("^metrics_http_listener=tcp-ipv4$$", cli_makefile)
        self.assertIn("^metrics_scrape_path=/metrics$$", cli_makefile)
        self.assertIn("^metrics_listen=tcp:127.0.0.1:9900$$", cli_makefile)
        self.assertIn("^client_api=pretraining-step-commit-v1$$", cli_makefile)
        self.assertIn("install-smoke: install", cli_makefile)
        self.assertIn("print-release-manifest", cli_makefile)
        self.assertIn("print-package-manifest", cli_makefile)
        self.assertIn("print-wire-schema", cli_makefile)
        self.assertIn("print-admin-output-schema", cli_makefile)
        self.assertIn("print-upgrade-rollback-policy", cli_makefile)
        self.assertIn("print-ops-certification-policy", cli_makefile)
        self.assertIn("print-alert-rules", cli_makefile)
        self.assertIn("print-api-abi-policy", cli_makefile)
        self.assertIn("print-compat-matrix", cli_makefile)
        self.assertIn("print-compat-baseline-v1", cli_makefile)
        self.assertIn("print-compat-old-new-matrix", cli_makefile)
        self.assertIn("wire_schema_manifest_checksum=0xce883650", release_manifest)
        self.assertIn("admin_output_schema=share/lingqu/mem_service/admin-output-schema.txt", release_manifest)
        self.assertIn("admin_output_schema_checksum=0x7021f4cf", release_manifest)
        self.assertIn("admin_output_format=text-kv", release_manifest)
        self.assertIn("admin_metric_prefix=lingqu_mem_service_", release_manifest)
        self.assertIn(
            "upgrade_rollback_policy=share/lingqu/mem_service/upgrade-rollback-policy.txt",
            release_manifest,
        )
        self.assertIn("package_format=installed-layout-v1", release_manifest)
        self.assertIn("package_manifest=share/lingqu/mem_service/package-manifest.txt", release_manifest)
        self.assertIn("package_manifest_checksum=0x1e9f6129", release_manifest)
        self.assertIn("installed_sdk_example_smoke=installed-sdk-example-smoke", release_manifest)
        self.assertIn("package_gate=package-fixtures", release_manifest)
        self.assertIn(
            "distributable_package=out/mem_service/"
            "linqu_mem_service-installed-layout-v1.tar",
            release_manifest,
        )
        self.assertIn("distributable_package_format=tar", release_manifest)
        self.assertIn("distributable_package_root=usr+etc", release_manifest)
        self.assertIn("distributable_package_gate=package-tarball-smoke", release_manifest)
        self.assertIn(
            "native_package=out/mem_service/linqu-mem-service_0.1.0-1_arm64.deb",
            release_manifest,
        )
        self.assertIn("native_package_format=deb", release_manifest)
        self.assertIn("native_package_arch=arm64", release_manifest)
        self.assertIn("native_package_gate=package-deb-smoke", release_manifest)
        self.assertIn(
            "native_package_runtime=not-executed-cross-compiled-arm64",
            release_manifest,
        )
        self.assertIn(
            "rpm_native_package=out/mem_service/linqu-mem-service-0.1.0-1.aarch64.rpm",
            release_manifest,
        )
        self.assertIn("rpm_native_package_format=rpm", release_manifest)
        self.assertIn("rpm_native_package_arch=aarch64", release_manifest)
        self.assertIn("rpm_native_package_gate=package-rpm-smoke", release_manifest)
        self.assertIn(
            "rpm_native_package_runtime=requires-linux-rpm-toolchain",
            release_manifest,
        )
        self.assertIn(
            "linux_ops_certification_smoke=linux-ops-certification-smoke",
            release_manifest,
        )
        self.assertIn(
            "linux_ops_upgrade_rollback_smoke=linux-ops-upgrade-rollback-smoke",
            release_manifest,
        )
        self.assertIn("upgrade_rollback_policy_checksum=0xcdcd3550", release_manifest)
        self.assertIn(
            "upgrade_rollback_runtime_gate=upgrade-rollback-runtime-fixtures",
            release_manifest,
        )
        self.assertIn("compat_runtime_gate=compat-runtime-fixtures", release_manifest)
        self.assertIn(
            "compat_old_server_runtime_gate=compat-old-server-runtime-fixtures",
            release_manifest,
        )
        self.assertIn("serving_fail_closed_matrix=certified", release_manifest)
        self.assertIn(
            "serving_fail_closed_gate=serving-fail-closed-fixtures",
            release_manifest,
        )
        self.assertIn("pretraining_fail_closed_matrix=certified", release_manifest)
        self.assertIn(
            "pretraining_fail_closed_gate=pretraining-fail-closed-fixtures",
            release_manifest,
        )
        self.assertIn("wire_payload_text_kv_format=text-kv", release_manifest)
        self.assertIn(
            "wire_payload_typed_binary_format=typed-binary-v1", release_manifest
        )
        self.assertIn(
            "wire_payload_typed_binary_gate=typed-payload-fixtures",
            release_manifest,
        )
        self.assertIn("upgrade_policy=current-version-only", release_manifest)
        self.assertIn("rollback_policy=current-version-only", release_manifest)
        self.assertIn("old_server_runtime_binary=certified", release_manifest)
        self.assertIn("api_abi_policy=share/lingqu/mem_service/api-abi-policy.txt", release_manifest)
        self.assertIn("api_abi_policy_checksum=0x5d95ae02", release_manifest)
        self.assertIn("client_api_version=1", release_manifest)
        self.assertIn("client_abi_version=1", release_manifest)
        self.assertIn("client_record_abi_size=744", release_manifest)
        self.assertIn("compat_matrix=share/lingqu/mem_service/compat-matrix.txt", release_manifest)
        self.assertIn("compat_matrix_checksum=0x1844c64d", release_manifest)
        self.assertIn("compat_baseline=share/lingqu/mem_service/compat-baseline-v1.txt", release_manifest)
        self.assertIn("compat_baseline_checksum=0xdac5b8d5", release_manifest)
        self.assertIn("compat_old_new_matrix=share/lingqu/mem_service/compat-old-new-matrix.txt", release_manifest)
        self.assertIn("compat_old_new_matrix_checksum=0x6509c49d", release_manifest)
        self.assertIn(
            "host_daemon_binary=libexec/lingqu/mem_service/linqu_mem_service_host",
            release_manifest,
        )
        self.assertIn("host_daemon_artifact_smoke=host-artifact-smoke", release_manifest)
        self.assertIn(
            "host_deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.host.service",
            release_manifest,
        )
        self.assertIn(
            "systemd_unit=lib/systemd/system/linqu_mem_service.service",
            release_manifest,
        )
        self.assertIn(
            "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service",
            release_manifest,
        )
        self.assertIn("deployment_smoke=deployment-fixtures", release_manifest)
        self.assertIn("host_service_manager_smoke=installed-host-service-manager-smoke", release_manifest)
        self.assertIn("host_service_manager_lifecycle=host-serve-config-ready-scrape-sigterm", release_manifest)
        self.assertIn("collector_smoke=collector-fixtures", release_manifest)
        self.assertIn("collector_integration_smoke=installed-host-collector-smoke", release_manifest)
        self.assertIn("collector_scrape_contract=prometheus-text-http-v0.0.4", release_manifest)
        self.assertIn(
            "alert_rules=share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml",
            release_manifest,
        )
        self.assertIn("alert_rules_checksum=0xbdff2246", release_manifest)
        self.assertIn("alert_rule_count=5", release_manifest)
        self.assertIn("alert_rules_gate=alert-fixtures", release_manifest)
        self.assertIn("alert_integration_smoke=alert-integration-fixtures", release_manifest)
        self.assertIn(
            "service_manager_lifecycle=serve-config-ready-scrape-sigterm",
            release_manifest,
        )
        self.assertIn("service_manager_shutdown=signal-clean-stop", release_manifest)
        self.assertIn("durable_backend=snapshot+journal", release_manifest)
        self.assertIn("durable_catalog=storage-root-v1", release_manifest)
        self.assertIn("durable_catalog_manifest=catalog/manifest.txt", release_manifest)
        self.assertIn("payload_block_backend=sealed-local-block-v1,sealed-chunked-block-v1,transport-loopback-block-v1,transport-tcp-block-v1", release_manifest)
        self.assertIn("remote_payload_block_backend=transport-loopback-block-v1,transport-tcp-block-v1", release_manifest)
        self.assertIn("remote_payload_block_data_gate=transport-block-fixtures", release_manifest)
        self.assertIn("remote_payload_network_transport=tcp-loopback-certified", release_manifest)
        self.assertIn("remote_payload_network_transport_gate=network-transport-block-fixtures", release_manifest)
        self.assertIn("remote_payload_network_transport_make_gate=network-transport-block-smoke", release_manifest)
        self.assertIn("durable_journal=store-path.journal", release_manifest)
        self.assertIn("metrics_listen_config=metrics_listen", release_manifest)
        self.assertIn("metrics_http_listener=tcp-ipv4", release_manifest)
        self.assertIn("metrics_scrape_path=/metrics", release_manifest)
        self.assertIn("config_schema_version=1", release_manifest)
        self.assertIn("config_schema=share/lingqu/mem_service/config/mem_service.conf.schema", release_manifest)
        self.assertIn("config_example=share/lingqu/mem_service/config/mem_service.example.conf", release_manifest)
        self.assertIn("runtime_config=etc/lingqu/mem_service/mem_service.conf", release_manifest)
        self.assertIn(
            "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf",
            release_manifest,
        )
        self.assertIn("deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service", release_manifest)
        self.assertIn("metrics_export_format=prometheus-text", release_manifest)
        self.assertIn("client_retry_policy=explicit-max-attempts-backoff", release_manifest)
        self.assertIn("client_api=pretraining-refs-v1", release_manifest)
        self.assertIn("client_api=pretraining-step-commit-v1", release_manifest)
        self.assertIn("operation=metrics:5", release_manifest)
        self.assertIn("operation=audit_log:10", release_manifest)
        self.assertIn("operation=export_snapshot:6", release_manifest)
        self.assertIn("operation=export_snapshot_page:7", release_manifest)
        self.assertIn("operation=restore_snapshot:8", release_manifest)
        self.assertIn("operation=restore_snapshot_page:9", release_manifest)
        self.assertIn("operation=inspect_object:18", release_manifest)
        self.assertIn(
            "example_source=share/lingqu/mem_service/examples/"
            "mem_service_serving_example.c",
            release_manifest,
        )
        self.assertIn(
            "example_source=share/lingqu/mem_service/examples/"
            "mem_service_pretraining_example.c",
            release_manifest,
        )
        self.assertIn("examples=2", cli_source)
        self.assertIn("host_artifacts=1", cli_source)
        self.assertIn("systemd_units=2", cli_source)
        self.assertIn("package_artifacts=4", cli_source)
        self.assertIn("config_artifacts=6", cli_source)
        self.assertIn("service_manager_lifecycle_smokes=1", cli_source)
        self.assertIn("host_service_manager_smokes=1", cli_source)
        self.assertIn("collector_smokes=1", cli_source)
        self.assertIn("alert_rule_artifacts=1", cli_source)
        self.assertIn("alert_integration_smokes=1", cli_source)
        self.assertIn("alert_rules_checksum=0x%08x", cli_source)
        self.assertIn("api_abi_policies=1", cli_source)
        self.assertIn("admin_output_schemas=1", cli_source)
        self.assertIn("admin_output_schema_checksum=0x%08x", cli_source)
        self.assertIn("upgrade_rollback_policies=1", cli_source)
        self.assertIn("upgrade_rollback_runtime_smokes=1", cli_source)
        self.assertIn("upgrade_rollback_policy_checksum=0x%08x", cli_source)
        self.assertIn("package_manifest_checksum=0x%08x", cli_source)
        self.assertIn("api_abi_policy_checksum=0x%08x", cli_source)
        self.assertIn("durable_catalogs=1", cli_source)
        self.assertIn("payload_block_backends=4", cli_source)
        self.assertIn("metrics_export_formats=1", cli_source)
        self.assertIn("metrics_http_listeners=1", cli_source)
        self.assertIn("client_retry_policies=1", cli_source)
        self.assertIn("client_api_profiles=2", cli_source)
        self.assertIn("operation_count=23", (CLI_DIR / "wire-schema.txt").read_text())
        self.assertIn("field_count=110", (CLI_DIR / "wire-schema.txt").read_text())
        self.assertIn("operation=audit_log:10", (CLI_DIR / "wire-schema.txt").read_text())
        self.assertIn("mem_service_admin_output_schema_version=1", admin_output_schema)
        self.assertIn("admin_command=metrics-export operation=metrics response=prometheus-text", admin_output_schema)
        self.assertIn("metrics_prometheus_prefix=lingqu_mem_service_", admin_output_schema)
        self.assertIn("metric_field=request_latency_max_ms type=gauge", admin_output_schema)
        self.assertIn("audit_record_delimiter=audit_begin/audit_end", admin_output_schema)
        self.assertIn("snapshot_page_field=next_index type=u64", admin_output_schema)
        self.assertIn("fail_closed_status=checksum_mismatch", admin_output_schema)
        self.assertIn("mem_service_upgrade_rollback_policy_version=1", upgrade_rollback_policy)
        self.assertIn("upgrade_policy=current-version-only", upgrade_rollback_policy)
        self.assertIn("rollback_policy=current-version-only", upgrade_rollback_policy)
        self.assertIn(
            "same_version_runtime_gate=upgrade-rollback-runtime-fixtures",
            upgrade_rollback_policy,
        )
        self.assertIn("old_server_runtime_binary=certified", upgrade_rollback_policy)
        self.assertIn(
            "new_client_old_server=certified",
            upgrade_rollback_policy,
        )
        self.assertIn(
            "required_gate=upgrade-rollback-runtime-fixtures",
            upgrade_rollback_policy,
        )
        self.assertIn("required_gate=package-fixtures", upgrade_rollback_policy)
        self.assertIn("required_gate=install-smoke", upgrade_rollback_policy)
        self.assertIn("mem_service_package_manifest_version=1", package_manifest)
        self.assertIn("package_format=installed-layout-v1", package_manifest)
        self.assertIn("artifact_format=tar", package_manifest)
        self.assertIn(
            "artifact_name=linqu_mem_service-installed-layout-v1.tar",
            package_manifest,
        )
        self.assertIn("artifact_root=usr+etc", package_manifest)
        self.assertIn("artifact_install_prefix=/usr", package_manifest)
        self.assertIn("artifact_contents=installed-layout-v1-root", package_manifest)
        self.assertIn("artifact_gate=package-tarball-smoke", package_manifest)
        self.assertIn("native_package_format=deb", package_manifest)
        self.assertIn(
            "native_package_name=linqu-mem-service_0.1.0-1_arm64.deb",
            package_manifest,
        )
        self.assertIn("native_package_arch=arm64", package_manifest)
        self.assertIn(
            "native_package_payload=debian-binary+control.tar.gz+data.tar.gz",
            package_manifest,
        )
        self.assertIn("native_package_gate=package-deb-smoke", package_manifest)
        self.assertIn(
            "native_package_runtime=not-executed-cross-compiled-arm64",
            package_manifest,
        )
        self.assertIn("rpm_package_format=rpm", package_manifest)
        self.assertIn(
            "rpm_package_name=linqu-mem-service-0.1.0-1.aarch64.rpm",
            package_manifest,
        )
        self.assertIn("rpm_package_arch=aarch64", package_manifest)
        self.assertIn("rpm_package_payload=rpm-cpio+metadata", package_manifest)
        self.assertIn("rpm_package_gate=package-rpm-smoke", package_manifest)
        self.assertIn(
            "rpm_package_runtime=requires-linux-rpm-toolchain",
            package_manifest,
        )
        self.assertIn("installed_file_count=35", package_manifest)
        self.assertIn("system_config_root=etc/lingqu/mem_service", package_manifest)
        self.assertIn("runtime_config=etc/lingqu/mem_service/mem_service.conf", package_manifest)
        self.assertIn(
            "runtime_config_source=share/lingqu/mem_service/config/mem_service.runtime.conf",
            package_manifest,
        )
        self.assertIn("host_runtime_config=etc/lingqu/mem_service/mem_service.host.conf", package_manifest)
        self.assertIn(
            "host_runtime_config_source=share/lingqu/mem_service/config/mem_service.host.runtime.conf",
            package_manifest,
        )
        self.assertIn("systemd_unit_root=lib/systemd/system", package_manifest)
        self.assertIn(
            "systemd_unit=lib/systemd/system/linqu_mem_service.service",
            package_manifest,
        )
        self.assertIn(
            "host_systemd_unit=lib/systemd/system/linqu_mem_service.host.service",
            package_manifest,
        )
        self.assertIn("file_class=runtime_config count=2", package_manifest)
        self.assertIn("file_class=systemd_units count=2", package_manifest)
        self.assertIn("required_gate_count=23", package_manifest)
        self.assertIn("required_gate=package-fixtures", package_manifest)
        self.assertIn("required_gate=remote-transport-evidence-fixtures", package_manifest)
        self.assertIn(
            "required_gate=upgrade-rollback-runtime-fixtures",
            package_manifest,
        )
        self.assertIn("required_gate=compat-runtime-fixtures", package_manifest)
        self.assertIn("required_gate=ops-certification-fixtures", package_manifest)
        self.assertIn("required_gate=ops-certification-evidence-fixtures", package_manifest)
        self.assertIn("required_gate=ops-certification-linux-ci-smoke", package_manifest)
        self.assertIn("required_gate=package-tarball-smoke", package_manifest)
        self.assertIn("required_gate=package-deb-smoke", package_manifest)
        self.assertIn("required_gate=package-rpm-smoke", package_manifest)
        self.assertIn("required_gate=installed-sdk-example-smoke", package_manifest)
        self.assertIn("contract=ops-certification-policy", package_manifest)
        self.assertIn("cross_version_upgrade=certified", package_manifest)
        self.assertIn(
            "mem_service_ops_certification_policy_version=1",
            ops_certification_policy,
        )
        self.assertIn("certification_status=not-certified", ops_certification_policy)
        self.assertIn(
            "admission_rule=fail-closed-until-external-evidence",
            ops_certification_policy,
        )
        self.assertIn(
            "evidence_schema=ops-certification-evidence-v1",
            ops_certification_policy,
        )
        self.assertIn(
            "evidence_generate=ops-certification-generate-evidence",
            ops_certification_policy,
        )
        self.assertIn(
            "evidence_ci_gate=ops-certification-linux-ci-smoke",
            ops_certification_policy,
        )
        self.assertIn(
            "evidence_gate=ops-certification-evidence-fixtures",
            ops_certification_policy,
        )
        self.assertIn(
            "external_gate=linux-systemd-service-smoke",
            ops_certification_policy,
        )
        self.assertIn(
            "external_gate=prometheus-alertmanager-rule-smoke",
            ops_certification_policy,
        )
        self.assertIn("external_gate=rpm-package-smoke", ops_certification_policy)
        self.assertIn("alert: LingquMemServiceDown", alert_rules)
        self.assertIn(
            "increase(lingqu_mem_service_fail_closed_count[5m]) > 0",
            alert_rules,
        )
        self.assertIn(
            "increase(lingqu_mem_service_checksum_mismatch_count[5m]) > 0",
            alert_rules,
        )
        self.assertIn("lingqu_mem_service_request_latency_max_ms > 100", alert_rules)
        self.assertIn("mem_service_api_abi_policy_version=1", api_abi_policy)
        self.assertIn("client_api_version=1", api_abi_policy)
        self.assertIn("client_abi_version=1", api_abi_policy)
        self.assertIn("client_record_abi_size=744", api_abi_policy)
        self.assertIn("old_client_new_server_policy=compatible-within-v1", api_abi_policy)
        self.assertIn(
            "new_client_old_server_policy=certified",
            api_abi_policy,
        )
        compat_matrix = (CLI_DIR / "compat-matrix.txt").read_text()
        self.assertIn("mem_service_compat_matrix_version=1", compat_matrix)
        self.assertIn("wire_version_current=1", compat_matrix)
        self.assertIn("wire_schema_manifest_checksum=0xce883650", compat_matrix)
        self.assertIn("idempotency_conflict_status=version_conflict", compat_matrix)
        self.assertIn("idempotency_persistence=store-journal-and-full-snapshot", compat_matrix)
        self.assertIn("audit_log_persistence=store-journal-and-full-snapshot", compat_matrix)
        self.assertIn("journal_scope=completed-idempotency-and-audit-events", compat_matrix)
        self.assertIn("journal_truncation_policy=threshold-compaction", compat_matrix)
        self.assertIn("compat_test=journal-fixtures", compat_matrix)
        self.assertIn("compat_test=deployment-fixtures", compat_matrix)
        compat_baseline = (CLI_DIR / "compat-baseline-v1.txt").read_text()
        self.assertIn("mem_service_compat_baseline_version=1", compat_baseline)
        self.assertIn("old_client_new_server=compatible-within-v1", compat_baseline)
        self.assertIn("new_client_old_server=certified", compat_baseline)
        self.assertIn(
            "baseline_payload=register_training_artifact:v1-training-step-compatible",
            compat_baseline,
        )
        compat_old_new = (CLI_DIR / "compat-old-new-matrix.txt").read_text()
        self.assertIn("mem_service_old_new_compat_matrix_version=1", compat_old_new)
        self.assertIn(
            "certified_pair=current-v1-client->old-v1-schema-profile",
            compat_old_new,
        )
        self.assertIn(
            "certified_pair=current-v1-client->old-v1-runtime-binary",
            compat_old_new,
        )
        self.assertIn(
            "certification_limit=none",
            compat_old_new,
        )
        self.assertIn(
            "evidence=compat-old-server-runtime-fixtures",
            compat_old_new,
        )
        self.assertIn("mem_service_config_schema_version=1", config_schema)
        self.assertIn("field=listen type=string", config_schema)
        self.assertIn("field=store type=string", config_schema)
        self.assertIn("field=storage_root type=string", config_schema)
        self.assertIn("field=backend type=enum values=snapshot,snapshot+journal", config_schema)
        self.assertIn("field=metrics_listen type=string", config_schema)
        self.assertIn("field=auth_mode type=enum values=none", config_schema)
        self.assertIn("listen=unix:/tmp/linqu_mem_service.sock", config_example)
        self.assertIn("store=/tmp/linqu_mem_service.store", config_example)
        self.assertIn("backend=snapshot+journal", config_example)
        self.assertIn("metrics_listen=tcp:127.0.0.1:9900", config_example)
        self.assertIn("auth_mode=none", config_example)
        self.assertIn("listen=unix:/run/lingqu/mem_service.sock", config_runtime)
        self.assertIn("store=/var/lib/lingqu/mem_service/store.snapshot", config_runtime)
        self.assertIn("storage_root=/var/lib/lingqu/mem_service", config_runtime)
        self.assertIn("backend=snapshot+journal", config_runtime)
        self.assertIn("metrics_listen=tcp:127.0.0.1:9900", config_runtime)
        self.assertIn("listen=unix:/run/lingqu/mem_service_host.sock", config_host_runtime)
        self.assertIn(
            "store=/var/lib/lingqu/mem_service_host/store.snapshot",
            config_host_runtime,
        )
        self.assertIn("storage_root=/var/lib/lingqu/mem_service_host", config_host_runtime)
        self.assertIn("metrics_listen=tcp:127.0.0.1:9901", config_host_runtime)
        self.assertIn(
            "ExecStart=/usr/bin/linqu_mem_service serve --config "
            "/etc/lingqu/mem_service/mem_service.conf",
            deploy_manifest,
        )
        self.assertIn(
            "ExecStart=/usr/libexec/lingqu/mem_service/linqu_mem_service_host "
            "serve --config /etc/lingqu/mem_service/mem_service.host.conf",
            host_deploy_manifest,
        )
        self.assertIn("RuntimeDirectory=lingqu", deploy_manifest)
        self.assertIn("StateDirectory=lingqu/mem_service", deploy_manifest)
        self.assertIn("RuntimeDirectory=lingqu", host_deploy_manifest)
        self.assertIn("StateDirectory=lingqu/mem_service_host", host_deploy_manifest)
        self.assertIn('#include "mem_service_client.h"', serving_example)
        self.assertIn("mem_service_client_register_prefix_entry", serving_example)
        self.assertIn("mem_service_client_publish_kv_segment", serving_example)
        self.assertIn("mem_service_client_publish_runtime_handoff", serving_example)
        self.assertIn("mem_service_client_register_execution_artifact", serving_example)
        self.assertIn("mem_service_wire_client_options_init", serving_example)
        self.assertIn("mem_service_client_init_with_options", serving_example)
        self.assertIn("retry_backoff_ms = 10", serving_example)
        self.assertIn("retry_on_timeout = 1", serving_example)
        self.assertIn("idempotency_key", serving_example)
        self.assertNotIn("mem_service_daemon", serving_example)
        self.assertNotIn("mem_service_core", serving_example)
        self.assertIn('#include "mem_service_client.h"', pretraining_example)
        self.assertIn("mem_service_client_training_ref", pretraining_example)
        self.assertIn("mem_service_client_publish_dataset_shard", pretraining_example)
        self.assertIn("mem_service_client_resolve_dataset_shard", pretraining_example)
        self.assertIn("mem_service_client_publish_sample_batch", pretraining_example)
        self.assertIn("mem_service_client_resolve_sample_batch", pretraining_example)
        self.assertIn("mem_service_client_publish_checkpoint", pretraining_example)
        self.assertIn("mem_service_client_resolve_checkpoint", pretraining_example)
        self.assertIn("mem_service_client_publish_gradient_bucket", pretraining_example)
        self.assertIn("mem_service_client_resolve_gradient_bucket", pretraining_example)
        self.assertIn("mem_service_client_publish_optimizer_state", pretraining_example)
        self.assertIn("mem_service_client_resolve_optimizer_state", pretraining_example)
        self.assertIn("mem_service_client_commit_training_step", pretraining_example)
        self.assertIn("mem_service_client_resolve_training_step", pretraining_example)
        self.assertIn("MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND", pretraining_example)
        self.assertIn("MEM_SERVICE_CLIENT_TRAINING_STEP_COMMIT_KIND", client_header)
        self.assertIn("#define MEM_SERVICE_CLIENT_API_VERSION 1U", client_header)
        self.assertIn("#define MEM_SERVICE_CLIENT_ABI_VERSION 1U", client_header)
        self.assertIn("#define MEM_SERVICE_CLIENT_RECORD_ABI_SIZE 744U", client_header)
        self.assertIn("mem_service_client_record_size_must_match_abi", client_header)
        self.assertIn('"training-step-commit"', client_header)
        self.assertIn("mem_service_client_commit_training_step", client_header)
        self.assertIn("mem_service_client_resolve_training_step", client_header)
        self.assertNotIn("mem_service_client_register_training_artifact", pretraining_example)
        self.assertIn("mem_service_wire_client_options_init", pretraining_example)
        self.assertIn("mem_service_client_init_with_options", pretraining_example)
        self.assertIn("retry_backoff_ms = 10", pretraining_example)
        self.assertIn("retry_on_timeout = 1", pretraining_example)
        self.assertIn("idempotency_key", pretraining_example)
        self.assertIn("dataset-shard", pretraining_example)
        self.assertIn("sample-batch", pretraining_example)
        self.assertIn("checkpoint", pretraining_example)
        self.assertIn("gradient-bucket", pretraining_example)
        self.assertIn("optimizer-state", pretraining_example)
        self.assertNotIn("mem_service_daemon", pretraining_example)
        self.assertNotIn("mem_service_core", pretraining_example)
        core_sources = re.search(
            r"MEM_SERVICE_CORE_SRCS :=(?P<body>.*?)MEM_SERVICE_QWEN3_ADAPTER_SRCS :=",
            cli_makefile,
            re.S,
        )
        self.assertIsNotNone(core_sources)
        self.assertNotIn("LLM_INFER", core_sources.group("body"))
        self.assertNotIn("MEM_SERVICE_QWEN3", core_sources.group("body"))
        self.assertNotIn("$(MEM_SERVICE)", core_sources.group("body"))
        self.assertIn("$(MEM_SERVICE_DAEMON)", core_sources.group("body"))
        self.assertIn("$(MEM_SERVICE_CLIENT)", core_sources.group("body"))
        self.assertIn("$(MEM_SERVICE_WIRE_CLIENT)", core_sources.group("body"))
        self.assertIn("MEM_SERVICE_QWEN3_ADAPTER_SRCS", cli_makefile)
        self.assertIn("$(LLM_INFER)", cli_makefile)
        self.assertIn("-DMEM_SERVICE_ENABLE_QWEN3_INSPECT", cli_makefile)
        self.assertTrue(SERVICE_DAEMON_C.exists())
        self.assertTrue(SERVICE_DAEMON_H.exists())
        self.assertTrue(SERVICE_WIRE_H.exists())
        self.assertTrue(SERVICE_CLIENT_C.exists())
        self.assertTrue(SERVICE_CLIENT_H.exists())
        self.assertTrue(SERVICE_WIRE_CLIENT_C.exists())
        self.assertTrue(SERVICE_WIRE_CLIENT_H.exists())
        self.assertTrue(SERVICE_WIRE_PAYLOAD_H.exists())
        self.assertTrue(SERVICE_WIRE_SCHEMA_H.exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.c").exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.h").exists())
        self.assertTrue((CLI_DIR / "admin-output-schema.txt").exists())
        self.assertTrue((CLI_DIR / "upgrade-rollback-policy.txt").exists())
        self.assertTrue(
            (CLI_DIR / "deploy" / "linqu_mem_service.prometheus-alerts.yml").exists()
        )
        self.assertTrue((CLI_DIR / "api-abi-policy.txt").exists())
        self.assertTrue((CLI_DIR / "compat-baseline-v1.txt").exists())
        self.assertTrue((CLI_DIR / "compat-matrix.txt").exists())
        self.assertTrue((CLI_DIR / "compat-old-new-matrix.txt").exists())
        self.assertTrue((CLI_DIR / "configs" / "mem_service.host.runtime.conf").exists())
        self.assertTrue((CLI_DIR / "release-manifest.txt").exists())
        self.assertIn("mem_service_release_manifest_version=1", release_manifest)
        self.assertIn("core_binary=bin/linqu_mem_service", release_manifest)
        self.assertIn("public_header=include/lingqu/mem_service/mem_service_client.h", release_manifest)
        self.assertIn("client_source=src/lingqu/mem_service/mem_service_client.c", release_manifest)
        self.assertIn("operation=query_training_artifact:97", release_manifest)
        self.assertIn("status=internal:10", release_manifest)
        self.assertFalse((ROOT / "apps" / "mem_service_demo").exists())

    def test_pretraining_worker_runtime_gate_covers_restart_and_conflict(self):
        daemon_runtime_test = SERVICE_DAEMON_RUNTIME_TEST.read_text()

        self.assertIn(
            "test_pretraining_workers_publish_resolve_and_recover_refs",
            daemon_runtime_test,
        )
        self.assertIn("mem_service_pretraining_worker", daemon_runtime_test)
        self.assertIn("mem_service_client_publish_dataset_shard", daemon_runtime_test)
        self.assertIn("mem_service_client_publish_sample_batch", daemon_runtime_test)
        self.assertIn("mem_service_client_publish_checkpoint", daemon_runtime_test)
        self.assertIn("mem_service_client_publish_gradient_bucket", daemon_runtime_test)
        self.assertIn("mem_service_client_publish_optimizer_state", daemon_runtime_test)
        self.assertIn("mem_service_client_commit_training_step", daemon_runtime_test)
        self.assertIn("mem_service_client_resolve_training_step", daemon_runtime_test)
        self.assertIn("test_cli_training_step_commit_barrier_round_trips_fail_closed",
                      daemon_runtime_test)
        self.assertIn("test_audit_log_tracks_training_step_commit_and_fail_closed_after_restart",
                      daemon_runtime_test)
        self.assertIn("audit_log_count", daemon_runtime_test)
        self.assertIn("pretraining_worker=worker0 ok", daemon_runtime_test)
        self.assertIn("pretraining_worker=worker1 ok", daemon_runtime_test)
        self.assertIn("pretraining_worker=commit-step ok", daemon_runtime_test)
        self.assertIn("pretraining_worker=resolve ok", daemon_runtime_test)
        self.assertIn("pretraining_worker=step-conflict ok", daemon_runtime_test)
        self.assertIn("training-step-commit", daemon_runtime_test)
        self.assertIn("pretraining_worker=conflict ok", daemon_runtime_test)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_STALE_REF", daemon_runtime_test)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH", daemon_runtime_test)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT", daemon_runtime_test)
        self.assertIn("idempotency_conflict_count", daemon_runtime_test)

    def test_record_caps_support_long_decode_runs(self):
        header = SERVICE_H.read_text()
        cluster_payload_contract = SERVICE_CLUSTER_PAYLOAD_CONTRACT_H.read_text()

        max_records = re.search(r"#define MEM_SERVICE_MAX_RECORDS\s+(\d+)U", header)
        cluster_records = re.search(
            r"#define MEM_SERVICE_CLUSTER_MAX_RECORDS\s+(\d+)",
            cluster_payload_contract,
        )

        self.assertIsNotNone(max_records)
        self.assertIsNotNone(cluster_records)
        self.assertGreaterEqual(int(max_records.group(1)), 1024)
        self.assertGreaterEqual(int(cluster_records.group(1)), 1024)

    def test_mem_service_has_stable_wire_and_unix_daemon_boundary(self):
        wire = SERVICE_WIRE_H.read_text()
        client = SERVICE_CLIENT_H.read_text()
        client_source = SERVICE_CLIENT_C.read_text()
        wire_client = SERVICE_WIRE_CLIENT_H.read_text()
        wire_client_source = SERVICE_WIRE_CLIENT_C.read_text()
        wire_payload = SERVICE_WIRE_PAYLOAD_H.read_text()
        wire_schema = SERVICE_WIRE_SCHEMA_H.read_text()
        daemon = SERVICE_DAEMON_C.read_text()
        daemon_header = SERVICE_DAEMON_H.read_text()
        service_header = SERVICE_H.read_text()
        cli_source = (CLI_DIR / "mem_service.c").read_text()

        self.assertIn("#define MEM_SERVICE_WIRE_MAGIC", wire)
        self.assertIn("#define MEM_SERVICE_WIRE_VERSION 1U", wire)
        self.assertIn("#define MEM_SERVICE_WIRE_HEADER_LEN 48U", wire)
        self.assertIn("struct mem_service_wire_header", wire)
        for operation in (
            "MEM_SERVICE_WIRE_OP_HEALTH",
            "MEM_SERVICE_WIRE_OP_READY",
            "MEM_SERVICE_WIRE_OP_STATUS",
            "MEM_SERVICE_WIRE_OP_LIST_RECORDS",
            "MEM_SERVICE_WIRE_OP_PUT_OBJECT",
            "MEM_SERVICE_WIRE_OP_GET_OBJECT",
            "MEM_SERVICE_WIRE_OP_REGISTER_PREFIX_ENTRY",
            "MEM_SERVICE_WIRE_OP_LOOKUP_PREFIX_ENTRY",
            "MEM_SERVICE_WIRE_OP_PUBLISH_KV_SEGMENT",
            "MEM_SERVICE_WIRE_OP_RESOLVE_KV_SEGMENT",
            "MEM_SERVICE_WIRE_OP_PUBLISH_RUNTIME_HANDOFF",
            "MEM_SERVICE_WIRE_OP_RESOLVE_RUNTIME_HANDOFF",
            "MEM_SERVICE_WIRE_OP_REGISTER_EXECUTION_ARTIFACT",
            "MEM_SERVICE_WIRE_OP_QUERY_EXECUTION_ARTIFACT",
            "MEM_SERVICE_WIRE_OP_REGISTER_TRAINING_ARTIFACT",
            "MEM_SERVICE_WIRE_OP_QUERY_TRAINING_ARTIFACT",
        ):
            self.assertIn(operation, wire)
        for status in (
            "MEM_SERVICE_WIRE_STATUS_OK",
            "MEM_SERVICE_WIRE_STATUS_NOT_FOUND",
            "MEM_SERVICE_WIRE_STATUS_STALE_REF",
            "MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH",
            "MEM_SERVICE_WIRE_STATUS_VERSION_CONFLICT",
            "MEM_SERVICE_WIRE_STATUS_INVALID_MODEL_BINDING",
            "MEM_SERVICE_WIRE_STATUS_INVALID_SESSION",
            "MEM_SERVICE_WIRE_STATUS_TIMEOUT",
            "MEM_SERVICE_WIRE_STATUS_CAPACITY_EXCEEDED",
            "MEM_SERVICE_WIRE_STATUS_UNSUPPORTED",
            "MEM_SERVICE_WIRE_STATUS_INTERNAL",
        ):
            self.assertIn(status, wire)
        self.assertIn("MEM_SERVICE_DEFAULT_UNIX_SOCKET", wire_client)
        self.assertIn("struct mem_service_client", client)
        self.assertIn("struct mem_service_client_record", client)
        self.assertIn("struct mem_service_client_object", client)
        self.assertIn("struct mem_service_client_block_entry", client)
        self.assertIn("struct mem_service_client_artifact", client)
        self.assertIn("struct mem_service_client_training_ref", client)
        self.assertIn("struct mem_service_client_training_ref_query", client)
        self.assertIn("mem_service_client_health", client)
        self.assertIn("mem_service_client_put_object", client)
        self.assertIn("mem_service_client_register_prefix_entry", client)
        self.assertIn("mem_service_client_publish_kv_segment", client)
        self.assertIn("mem_service_client_publish_runtime_handoff", client)
        self.assertIn("mem_service_client_register_execution_artifact", client)
        self.assertIn("mem_service_client_register_training_artifact", client)
        self.assertIn("mem_service_client_publish_dataset_shard", client)
        self.assertIn("mem_service_client_resolve_dataset_shard", client)
        self.assertIn("mem_service_client_publish_sample_batch", client)
        self.assertIn("mem_service_client_resolve_sample_batch", client)
        self.assertIn("mem_service_client_publish_checkpoint", client)
        self.assertIn("mem_service_client_resolve_checkpoint", client)
        self.assertIn("mem_service_client_publish_gradient_bucket", client)
        self.assertIn("mem_service_client_resolve_gradient_bucket", client)
        self.assertIn("mem_service_client_publish_optimizer_state", client)
        self.assertIn("mem_service_client_resolve_optimizer_state", client)
        self.assertIn("struct mem_service_wire_client_options", client)
        self.assertIn("mem_service_client_init_with_options", client)
        self.assertIn("mem_service_wire_client_options_init", client_source)
        self.assertIn('#include "mem_service_wire_client.h"', client_source)
        self.assertIn('#include "mem_service_wire_payload.h"', client_source)
        self.assertIn("mem_service_send_unix_request_with_options", client_source)
        self.assertIn("mem_service_client_publish_training_ref", client_source)
        self.assertIn("mem_service_client_resolve_training_ref", client_source)
        self.assertIn("mem_service_wire_payload_append_field", client_source)
        self.assertIn("mem_service_wire_payload_get_string", client_source)
        self.assertNotIn('#include "mem_service_daemon.h"', client_source)
        self.assertNotIn('#include "mem_service_core.h"', client_source)
        self.assertIn("mem_service_wire_status_name", wire_client)
        self.assertIn("mem_service_default_unix_socket_spec", wire_client)
        self.assertIn("mem_service_send_unix_request", wire_client)
        self.assertIn("mem_service_send_unix_request_with_options", wire_client)
        self.assertIn("struct mem_service_wire_client_options", wire_client)
        self.assertIn("MEM_SERVICE_WIRE_CLIENT_DEFAULT_MAX_ATTEMPTS", wire_client)
        self.assertIn("MEM_SERVICE_WIRE_CLIENT_MAX_ATTEMPTS", wire_client)
        self.assertIn("max_attempts", wire_client)
        self.assertIn("retry_backoff_ms", wire_client)
        self.assertIn("retry_on_timeout", wire_client)
        self.assertIn("mem_service_wire_client_options_init", wire_client)
        self.assertIn("const char *payload_in", wire_client)
        self.assertIn("mem_service_send_unix_request", wire_client_source)
        self.assertIn("mem_service_send_unix_request_with_options", wire_client_source)
        self.assertIn("SO_RCVTIMEO", wire_client_source)
        self.assertIn("SO_SNDTIMEO", wire_client_source)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_TIMEOUT", wire_client_source)
        self.assertIn("mem_service_client_effective_max_attempts", wire_client_source)
        self.assertIn("mem_service_client_should_retry", wire_client_source)
        self.assertIn("mem_service_client_sleep_ms", wire_client_source)
        self.assertIn("nanosleep", wire_client_source)
        self.assertIn("const char *payload_in", wire_client_source)
        self.assertIn("socket(AF_UNIX, SOCK_STREAM, 0)", wire_client_source)
        self.assertIn("connect(fd", wire_client_source)
        self.assertIn("mem_service_client_read_full", wire_client_source)
        self.assertIn("mem_service_client_write_full", wire_client_source)
        self.assertNotIn('#include "mem_service_core.h"', wire_client_source)
        self.assertIn("struct mem_service_wire_payload_view", wire_payload)
        self.assertIn("struct mem_service_wire_payload_field", wire_payload)
        self.assertIn("MEM_SERVICE_WIRE_PAYLOAD_FIELD_STRING", wire_payload)
        self.assertIn("MEM_SERVICE_WIRE_PAYLOAD_FIELD_U32", wire_payload)
        self.assertIn("MEM_SERVICE_WIRE_PAYLOAD_FIELD_U64", wire_payload)
        self.assertIn("mem_service_wire_payload_get_string", wire_payload)
        self.assertIn("mem_service_wire_payload_get_u64_checked", wire_payload)
        self.assertIn("mem_service_wire_payload_get_u32", wire_payload)
        self.assertIn("mem_service_wire_payload_append_field", wire_payload)
        self.assertIn("mem_service_wire_payload_validate_schema", wire_payload)
        self.assertIn("#define MEM_SERVICE_WIRE_SCHEMA_VERSION 1U", wire_schema)
        self.assertIn("struct mem_service_wire_operation_schema", wire_schema)
        self.assertIn("struct mem_service_wire_payload_oneof", wire_schema)
        self.assertIn("mem_service_wire_schema_for_operation", wire_schema)
        self.assertIn("mem_service_wire_schema_validate_payload", wire_schema)
        self.assertIn("mem_service_wire_object_put_fields", wire_schema)
        self.assertIn("mem_service_wire_artifact_query_fields", wire_schema)
        self.assertIn("mem_service_wire_kv_resolve_oneofs", wire_schema)
        self.assertIn('"key"', wire_schema)
        self.assertIn('"block_hash"', wire_schema)
        self.assertIn('#include "mem_service_wire_payload.h"', daemon)
        self.assertIn('#include "mem_service_wire_schema.h"', daemon)
        self.assertIn(
            '#include "components/mem_service/mem_service_wire_payload.h"',
            cli_source,
        )
        self.assertIn("--timeout-ms", cli_source)
        self.assertIn("parse_client_options", cli_source)
        self.assertIn("mem_service_send_unix_request_with_options", cli_source)
        self.assertIn("mem_service_wire_payload_append_field", cli_source)
        self.assertIn("mem_service_run_unix_daemon", daemon_header)
        self.assertIn("mem_service_run_unix_daemon_with_store", daemon_header)
        self.assertIn("mem_service_run_wire_fixture_check", daemon_header)
        self.assertIn("mem_service_run_store_fixture_check", daemon_header)
        self.assertNotIn("mem_service_send_unix_request", daemon_header)
        self.assertNotIn("mem_service_client_", daemon_header)
        self.assertIn('#include "mem_service_core.h"', daemon)
        self.assertIn("socket(AF_UNIX, SOCK_STREAM, 0)", daemon)
        self.assertIn("bind(server_fd", daemon)
        self.assertIn("listen(server_fd, 16)", daemon)
        self.assertIn("accept(server_fd", daemon)
        self.assertIn("sigaction(SIGINT", daemon)
        self.assertIn("sigaction(SIGTERM", daemon)
        self.assertIn("MEM_SERVICE_WIRE_OP_HEALTH", daemon)
        self.assertIn("MEM_SERVICE_WIRE_OP_READY", daemon)
        self.assertIn("MEM_SERVICE_WIRE_OP_STATUS", daemon)
        self.assertIn("MEM_SERVICE_WIRE_OP_LIST_RECORDS", daemon)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_UNSUPPORTED", daemon)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_CHECKSUM_MISMATCH", daemon)
        self.assertIn("mem_service_init(&svc, true, true, true)", daemon)
        self.assertIn('#include "mem_service_record_table.h"', daemon)
        self.assertIn("mem_service_read_payload", daemon)
        self.assertIn("mem_service_put_object", daemon)
        self.assertIn("mem_service_get_object", daemon)
        self.assertIn("mem_service_inspect_object", daemon)
        self.assertIn("mem_service_export_snapshot", daemon)
        self.assertIn("mem_service_export_snapshot_page", daemon)
        self.assertIn("mem_service_restore_snapshot", daemon)
        self.assertIn("mem_service_restore_snapshot_page", daemon)
        self.assertIn("mem_service_register_prefix", daemon)
        self.assertIn("mem_service_lookup_prefix", daemon)
        self.assertIn("mem_service_publish_kv", daemon)
        self.assertIn("mem_service_resolve_kv", daemon)
        self.assertIn("mem_service_store_artifact", daemon)
        self.assertIn("mem_service_query_artifact", daemon)
        self.assertIn("mem_service_status", daemon)
        self.assertIn("mem_service_list_records", daemon)
        self.assertIn("mem_service_record_kind_name", daemon)
        self.assertIn("record_count=%zu", daemon)
        self.assertIn('MEM_SERVICE_STORE_MAGIC "mem_service_store_v1"', daemon)
        self.assertIn("mem_service_load_store", daemon)
        self.assertIn("mem_service_save_store", daemon)
        self.assertIn("mem_service_operation_mutates", daemon)
        self.assertIn("durable_store_save_failed", daemon)
        self.assertIn("record_begin", daemon)
        self.assertIn("MEM_SERVICE_RECORD_RUNTIME_HANDOFF", daemon)
        self.assertIn("MEM_SERVICE_RECORD_EXECUTION_ARTIFACT", daemon)
        self.assertIn("MEM_SERVICE_RECORD_TRAINING_ARTIFACT", daemon)
        self.assertIn("MEM_SERVICE_WIRE_STATUS_STALE_REF", daemon)
        self.assertIn("mem_service_run_wire_fixture_check", daemon)
        self.assertIn("mem_service_run_store_fixture_check", daemon)
        self.assertIn("mem_service_wire_schema_for_operation", daemon)
        self.assertIn("mem_service_wire_schema_validate_payload", daemon)
        self.assertNotIn("mem_service_object_put_schema", daemon)
        self.assertNotIn("mem_service_artifact_query_schema", daemon)
        self.assertIn("mem_service_wire_payload_view_from_cstr(payload)", daemon)
        self.assertIn("offsetof(struct mem_service_wire_header, request_id)", daemon)
        for fixture_name in (
            "health_request",
            "ready_request",
            "status_request",
            "list_records_request",
            "put_object_request",
            "get_object_request",
            "register_prefix_request",
            "lookup_prefix_request",
            "publish_kv_request",
            "resolve_kv_request",
            "publish_runtime_handoff_request",
            "resolve_runtime_handoff_request",
            "register_execution_artifact_request",
            "query_execution_artifact_request",
            "register_training_artifact_request",
            "query_training_artifact_request",
            "metrics_request",
            "export_snapshot_request",
            "export_snapshot_page_request",
            "inspect_object_request",
            "restore_snapshot_request",
            "restore_snapshot_page_request",
            "audit_log_request",
        ):
            self.assertIn(fixture_name, daemon)
        for response_fixture_name in (
            "health_response",
            "ready_response",
            "put_object_response",
            "get_object_response",
            "register_prefix_response",
            "lookup_prefix_response",
            "publish_kv_response",
            "resolve_kv_response",
            "publish_runtime_handoff_response",
            "resolve_runtime_handoff_response",
            "register_execution_artifact_response",
            "query_execution_artifact_response",
            "register_training_artifact_response",
            "query_training_artifact_response",
            "status_response",
            "list_records_response",
            "metrics_response",
            "export_snapshot_response",
            "export_snapshot_page_response",
            "inspect_object_response",
            "restore_snapshot_response",
            "restore_snapshot_page_response",
            "audit_log_response",
        ):
            self.assertIn(response_fixture_name, daemon)
        for checksum in (
            "0x4e6f0ab1U",
            "0x099d6fbeU",
            "0x8a6bc143U",
            "0x112e24c8U",
            "0xab96fa3cU",
            "0x7b036ca5U",
            "0x9bdd9444U",
            "0x3e772698U",
            "0xe44a9059U",
            "0xf601bf3fU",
            "0x70ed07a5U",
            "0x7ccb46a2U",
            "0x663437afU",
            "0xa2b94d99U",
            "0xe87e631eU",
            "0x4f16a0c9U",
            "0x29d62b8bU",
            "0x2c6ac21eU",
            "0x6454ba82U",
            "0x8d9812a7U",
            "0xde8843f2U",
            "0x3f4609a1U",
            "0x3ae50a76U",
            "0x3fc9bd20U",
            "0x4c66d23cU",
            "0xabb21009U",
            "0x1b337d88U",
            "0xa5654285U",
            "0xdaa065aeU",
            "0xfe23a8a2U",
            "0xe54d9bffU",
            "0x802c9350U",
            "0xaac8ac2bU",
        ):
            self.assertIn(checksum, daemon)
        self.assertIn("MEM_SERVICE_METRIC_LATENCY_BUCKET_COUNT", service_header)
        self.assertIn("MEM_SERVICE_MAX_IDEMPOTENCY_RECORDS", service_header)
        self.assertIn("struct mem_service_idempotency_record", service_header)
        self.assertIn("idempotency_key", wire_schema)
        self.assertIn("idempotency_key", client)
        self.assertIn("idempotency_key", client_source)
        self.assertIn("--idempotency-key", cli_source)
        self.assertIn("append_idempotency_payload_field", cli_source)
        self.assertIn("mem_service_try_idempotency_replay", daemon)
        self.assertIn("mem_service_store_import_idempotency", daemon)
        self.assertIn("mem_service_save_idempotency_record", daemon)
        self.assertIn("mem_service_append_snapshot_idempotency_text", daemon)
        self.assertIn("idempotency_begin", daemon)
        self.assertIn("response_line=", daemon)
        self.assertIn("idempotency_replay_count", daemon)
        self.assertIn("idempotency_conflict_count", daemon)
        self.assertIn("request_latency_total_ms", daemon)
        self.assertIn("request_latency_le_1ms_count", daemon)
        self.assertIn("request_latency_gt_100ms_count", daemon)
        self.assertIn("payload_fixtures=%zu", daemon)
        self.assertIn("response_fixtures=%zu", daemon)
        self.assertIn("mem_service_expect_response_fixture", daemon)
        self.assertIn('mem_service_expect_u32("op_status", MEM_SERVICE_WIRE_OP_STATUS, 3)', daemon)
        self.assertIn(
            'mem_service_expect_u32("op_list_records", MEM_SERVICE_WIRE_OP_LIST_RECORDS, 4)',
            daemon,
        )
        self.assertIn('mem_service_expect_u32("op_metrics", MEM_SERVICE_WIRE_OP_METRICS, 5)', daemon)
        self.assertIn(
            'mem_service_expect_u32("op_export_snapshot",',
            daemon,
        )
        self.assertIn(
            'mem_service_expect_u32("op_export_snapshot_page",',
            daemon,
        )
        self.assertIn(
            'mem_service_expect_u32("op_restore_snapshot",',
            daemon,
        )
        self.assertIn(
            'mem_service_expect_u32("op_restore_snapshot_page",',
            daemon,
        )
        self.assertIn(
            'mem_service_expect_u32("op_inspect_object",',
            daemon,
        )
        self.assertIn("mem_service_record_operation_metrics", daemon)
        self.assertIn("mem_service_payload_get_string", daemon)
        self.assertIn('status=stopped', daemon)
        self.assertNotIn("qwen3", daemon.lower())

    def test_qwen3_record_kinds_are_adapter_aliases_not_core_enum_names(self):
        header = SERVICE_H.read_text()
        core_header = SERVICE_CORE_H.read_text()
        qwen3_header = SERVICE_QWEN3_H.read_text()
        cluster_read = SERVICE_CLUSTER_READ_C.read_text()
        cluster_observe = SERVICE_CLUSTER_OBSERVE_C.read_text()

        self.assertIn('#include "mem_service.h"', core_header)
        self.assertNotIn("qwen3", core_header.lower())
        self.assertNotIn("MEM_SERVICE_RECORD_QWEN3", header)
        self.assertIn("MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT = 9", header)
        self.assertIn("MEM_SERVICE_RECORD_MODEL_ENGRAM_HISTORY = 10", header)
        self.assertIn("MEM_SERVICE_RECORD_MODEL_ENGRAM_CANDIDATES = 11", header)
        self.assertIn("MEM_SERVICE_RECORD_MODEL_ENGRAM_SELECTED = 12", header)
        self.assertIn("MEM_SERVICE_RECORD_MODEL_ENGRAM_STATE = 13", header)
        self.assertIn("MEM_SERVICE_RECORD_RUNTIME_HANDOFF = 14", header)
        self.assertIn("MEM_SERVICE_RECORD_EXECUTION_ARTIFACT = 15", header)
        self.assertIn("MEM_SERVICE_RECORD_TRAINING_ARTIFACT = 16", header)
        self.assertIn(
            "#define MEM_SERVICE_RECORD_QWEN3_TOKEN_RESULT "
            "MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT",
            qwen3_header,
        )
        self.assertIn(
            "#define MEM_SERVICE_RECORD_QWEN3_ENGRAM_STATE "
            "MEM_SERVICE_RECORD_MODEL_ENGRAM_STATE",
            qwen3_header,
        )
        self.assertIn("MEM_SERVICE_RECORD_MODEL_TOKEN_RESULT", cluster_observe)
        self.assertIn("MEM_SERVICE_RECORD_TRAINING_ARTIFACT", cluster_read)
        self.assertIn("MEM_SERVICE_RECORD_TRAINING_ARTIFACT", cluster_observe)
        self.assertNotIn("MEM_SERVICE_RECORD_QWEN3", cluster_read)
        self.assertNotIn("MEM_SERVICE_RECORD_QWEN3", cluster_observe)

    def test_internal_runtime_contract_is_split_from_service_main(self):
        source = SERVICE_C.read_text()
        internal_header = SERVICE_INTERNAL_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_internal.h"', source)
        self.assertIn('#include "mem_service_cluster_payload_contract.h"', internal_header)
        self.assertIn('#include "mem_service_compiler.h"', internal_header)
        self.assertIn('#include "mem_service_guest_runtime.h"', internal_header)
        self.assertIn('#include "mem_service_object_contract.h"', internal_header)
        self.assertIn('#include "mem_service_qwen3_placement.h"', internal_header)
        self.assertIn('#include "mem_service_qwen3_record_policy.h"', internal_header)
        self.assertIn('#include "mem_service_runtime_config.h"', internal_header)
        self.assertIn("private include aggregate", readme)
        self.assertIn("private compatibility shims", readme)
        self.assertNotRegex(
            source,
            r"#define MEM_SERVICE_CLUSTER_MAX_RECORDS\s+\d+",
        )
        self.assertNotRegex(
            source,
            r"struct mem_service_cluster_runtime\s*\{",
        )
        self.assertNotRegex(
            source,
            r"static long mem_service_env_wait_ms_or_default"
            r"\s*\(",
        )
        self.assertNotRegex(
            internal_header,
            r"static long mem_service_env_wait_ms_or_default"
            r"\s*\(",
        )
        self.assertNotRegex(
            internal_header,
            r"struct mem_service_qwen3_layer_range_placement\s*\{",
        )
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_MAYBE_UNUSED",
        )
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS",
        )

    def test_compiler_annotations_are_split_from_internal_contract(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        compiler_header = SERVICE_COMPILER_H.read_text()
        cluster_read = SERVICE_CLUSTER_READ_C.read_text()
        cluster_utils = SERVICE_CLUSTER_UTILS_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_compiler.h"', internal_header)
        self.assertIn("MEM_SERVICE_MAYBE_UNUSED", compiler_header)
        self.assertIn('#include "mem_service_compiler.h"', cluster_read)
        self.assertIn('#include "mem_service_compiler.h"', cluster_utils)
        self.assertIn("local compiler annotations", readme)
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_MAYBE_UNUSED",
        )

    def test_qwen3_record_policy_is_split_from_internal_contract(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        qwen3_policy = SERVICE_QWEN3_RECORD_POLICY_H.read_text()
        qwen3_records = SERVICE_QWEN3_RECORDS_C.read_text()
        qwen3_record_contract = SERVICE_QWEN3_RECORDS_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_qwen3_record_policy.h"', internal_header)
        self.assertIn("MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS", qwen3_policy)
        self.assertIn('#include "mem_service_qwen3_records.h"', qwen3_records)
        self.assertIn('#include "mem_service_qwen3_record_policy.h"', qwen3_record_contract)
        self.assertIn("Qwen3 runtime record retention", readme)
        self.assertIn("model adapter record policy", readme)
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS",
        )

    def test_runtime_config_contract_is_split_from_internal_contract(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        runtime_config = SERVICE_RUNTIME_CONFIG_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_runtime_config.h"', internal_header)
        self.assertIn("MEM_SERVICE_CLUSTER_WAIT_MS", runtime_config)
        self.assertIn("MEM_SERVICE_OBMM_SERVICE_WAIT_MS", runtime_config)
        self.assertIn("mem_service_env_wait_ms_or_default", runtime_config)
        self.assertIn("mem_service_run_id_from_env", runtime_config)
        self.assertIn("mem_service_qwen3_runtime_range_wait_ms", runtime_config)
        self.assertIn("runtime wait defaults", readme)
        self.assertIn("neutral run-id resolution", readme)
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_CLUSTER_WAIT_MS\s+\d+L",
        )

    def test_qwen3_placement_contract_is_split_from_internal_contract(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        qwen3_placement = SERVICE_QWEN3_PLACEMENT_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_qwen3_placement.h"', internal_header)
        self.assertIn("struct mem_service_qwen3_layer_range_placement", qwen3_placement)
        self.assertIn("uint32_t owner_node", qwen3_placement)
        self.assertIn("bool terminal", qwen3_placement)
        self.assertIn("Qwen3 layer-range placement", readme)
        self.assertIn("runtime range, KV, and object handoff flows", readme)
        self.assertNotRegex(
            internal_header,
            r"struct mem_service_qwen3_layer_range_placement\s*\{",
        )

    def test_cluster_payload_contract_is_split_for_host_guest_reuse(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        cluster_payload_contract = SERVICE_CLUSTER_PAYLOAD_CONTRACT_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_payload_contract.h"', internal_header)
        self.assertIn("MEM_SERVICE_CLUSTER_MAX_RECORDS", cluster_payload_contract)
        self.assertIn("MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC", cluster_payload_contract)
        self.assertIn("struct mem_service_cluster_payload", cluster_payload_contract)
        self.assertIn(
            "struct mem_service_cluster_payload_compact_summary",
            cluster_payload_contract,
        )
        self.assertIn("cluster metadata payload wire format", readme)
        self.assertIn("guest and host service deployments", readme)
        self.assertNotRegex(
            internal_header,
            r"struct mem_service_cluster_payload\s*\{",
        )
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_CLUSTER_PAYLOAD_MAGIC\s+0x",
        )

    def test_guest_runtime_contract_is_split_from_internal_contract(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        guest_runtime = SERVICE_GUEST_RUNTIME_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_guest_runtime.h"', internal_header)
        self.assertIn("MEM_SERVICE_CLUSTER_MAX_NODES", guest_runtime)
        self.assertIn("MEM_SERVICE_CLUSTER_QUEUE_DEPTH", guest_runtime)
        self.assertIn("struct mem_service_cluster_runtime", guest_runtime)
        self.assertIn("struct mem_service_cluster_slot", guest_runtime)
        self.assertIn("struct obmm_spsc_queue *ingress_queues", guest_runtime)
        self.assertIn("guest OBMM cluster runtime state", readme)
        self.assertIn("mapped slots, queue descriptors", readme)
        self.assertNotRegex(
            internal_header,
            r"struct mem_service_cluster_runtime\s*\{",
        )
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_CLUSTER_QUEUE_DEPTH\s+\d+",
        )

    def test_obmm_object_contract_is_split_for_host_guest_reuse(self):
        internal_header = SERVICE_INTERNAL_H.read_text()
        object_contract = SERVICE_OBJECT_CONTRACT_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_object_contract.h"', internal_header)
        self.assertIn("MEM_SERVICE_OBMM_KIND_WEIGHT_TILE", object_contract)
        self.assertIn("MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT", object_contract)
        self.assertIn("MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES", object_contract)
        self.assertIn("MEM_SERVICE_OBMM_QWEN3_ENGRAM_STATE_BYTES", object_contract)
        self.assertIn("device-independent OBMM object", readme)
        self.assertIn("guest and host service deployments", readme)
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_OBMM_KIND_HIDDEN_RANGE_RUNTIME_OUTPUT\s+\d+U",
        )
        self.assertNotRegex(
            internal_header,
            r"#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES\s+0x",
        )

    def test_full_record_table_recycles_old_qwen3_runtime_records(self):
        source = (
            SERVICE_C.read_text()
            + "\n"
            + SERVICE_QWEN3_RECORDS_C.read_text()
            + "\n"
            + SERVICE_OBMM_OBJECTS_C.read_text()
        )

        self.assertIn("MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS", source)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", source)
        self.assertIn('strstr(key, "decode-step")', source)
        self.assertIn('strstr(key, "/step/")', source)
        self.assertIn("rec = mem_service_alloc_record(svc);", source)
        self.assertIn("rec = mem_service_recycle_qwen3_runtime_record(svc, key);", source)

    def test_record_table_helpers_are_split_from_main_service_translation_unit(self):
        source = SERVICE_C.read_text()
        record_table = SERVICE_RECORD_TABLE_H.read_text()
        records = SERVICE_RECORDS_C.read_text()
        qwen3_records = SERVICE_QWEN3_RECORDS_C.read_text()
        qwen3_record_contract = SERVICE_QWEN3_RECORDS_H.read_text()
        metadata = SERVICE_METADATA_C.read_text()
        obmm_objects = SERVICE_OBMM_OBJECTS_C.read_text()
        qwen3_runtime = SERVICE_QWEN3_RUNTIME_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_record_table.h"', source)
        self.assertNotIn('#include "mem_service_records.inc"', source)
        self.assertNotIn('#include "mem_service_qwen3_records.inc"', source)
        self.assertIn('#include "mem_service_qwen3_records.h"', source)
        self.assertIn('#include "mem_service.h"', record_table)
        self.assertIn("mem_service_alloc_record", records)
        self.assertIn("mem_service_find_record", records)
        self.assertIn("mem_service_alloc_record", record_table)
        self.assertIn("mem_service_find_record", record_table)
        self.assertIn("mem_service_add_member", record_table)
        self.assertIn('#include "mem_service_record_table.h"', records)
        self.assertNotIn("static struct mem_service_record *mem_service_alloc_record", record_table)
        self.assertNotIn("static struct mem_service_record *mem_service_find_record", record_table)
        self.assertIn('#include "mem_service_record_table.h"', metadata)
        self.assertIn('#include "mem_service_record_table.h"', obmm_objects)
        self.assertIn('#include "mem_service_record_table.h"', qwen3_runtime)
        self.assertIn('#include "mem_service_qwen3_records.h"', source)
        self.assertIn('#include "mem_service_qwen3_records.h"', qwen3_records)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", qwen3_record_contract)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", qwen3_records)
        self.assertIn("mem_service_qwen3_key_decode_step", qwen3_records)
        self.assertNotIn("mem_service_recycle_qwen3_runtime_record", records)
        self.assertIn("standalone core translation unit", readme)
        self.assertIn("Qwen3 streaming runtime record", readme)
        self.assertIn("standalone model-adapter translation unit", readme)
        self.assertIn("private core record-table helper\n  contract", readme)
        self.assertIn("private Qwen3 record recycling\n  helper contract", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_records.inc").exists())
        self.assertNotRegex(
            source,
            r"static struct mem_service_record \*mem_service_alloc_record"
            r"\(struct mem_service \*svc\)\s*\{",
        )
        self.assertNotRegex(
            source,
            r"static struct mem_service_record \*mem_service_alloc_record"
            r"\(struct mem_service \*svc\);",
        )
        self.assertNotRegex(
            source,
            r"static struct mem_service_record \*mem_service_recycle_qwen3_runtime_record",
        )

    def test_key_construction_helpers_are_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        keys = SERVICE_KEYS_C.read_text()
        keys_contract = SERVICE_KEYS_H.read_text()
        metadata = SERVICE_METADATA_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_keys.inc"', source)
        self.assertIn('#include "mem_service_keys.h"', keys)
        self.assertIn('#include "mem_service.h"', keys_contract)
        self.assertIn("#include <stdint.h>", keys)
        self.assertIn("#include <string.h>", keys)
        self.assertIn("mem_service_build_two_part_key", keys)
        self.assertIn("mem_service_build_prefix_key_from_parts_checked", keys)
        self.assertIn("mem_service_build_block_key_from_hash_checked", keys)
        self.assertIn("mem_service_build_prefix_key", keys_contract)
        self.assertIn("mem_service_build_group_key", keys_contract)
        self.assertIn("mem_service_build_block_key", keys_contract)
        self.assertIn('#include "mem_service_keys.h"', metadata)
        self.assertIn("Productization Split Contract", readme)
        self.assertIn("guest component and as a host-side service", readme)
        self.assertIn("standalone core translation unit", readme)
        self.assertIn("private key construction helper contract", readme)
        self.assertNotIn("mem_service_internal.h", keys)
        self.assertNotIn("mem_service_guest_runtime.h", keys)
        self.assertFalse((SERVICE_DIR / "mem_service_keys.inc").exists())
        self.assertNotRegex(
            source,
            r"static int mem_service_build_two_part_key"
            r"\(const char \*prefix,",
        )
        self.assertNotRegex(
            keys_contract,
            r"static int mem_service_build_two_part_key",
        )

    def test_object_ref_helpers_are_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        object_refs = SERVICE_OBJECT_REFS_C.read_text()
        object_refs_contract = SERVICE_OBJECT_REFS_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_object_refs.inc"', source)
        self.assertIn('#include "mem_service_object_refs.h"', source)
        self.assertIn('#include "mem_service_object_refs.h"', object_refs)
        self.assertIn('#include "mem_service.h"', object_refs_contract)
        self.assertIn("#include <stdint.h>", object_refs)
        self.assertIn("#include <string.h>", object_refs)
        self.assertIn("mem_service_checksum_bytes", object_refs)
        self.assertIn("mem_service_checksum_bytes", object_refs_contract)
        self.assertIn("mem_service_record_to_lingqu_obmm_ref", object_refs)
        self.assertIn("object-reference projection", readme)
        self.assertIn("private checksum/object-reference", readme)
        self.assertIn("standalone core", readme)
        self.assertNotIn("mem_service_internal.h", object_refs)
        self.assertNotIn("mem_service_guest_runtime.h", object_refs)
        self.assertNotIn("mem_service_internal.h", object_refs_contract)
        self.assertNotIn("mem_service_guest_runtime.h", object_refs_contract)
        self.assertFalse((SERVICE_DIR / "mem_service_object_refs.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_record_to_lingqu_obmm_ref"
            r"\(const struct mem_service_record \*record,",
        )

    def test_obmm_object_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        obmm_objects = SERVICE_OBMM_OBJECTS_C.read_text()
        obmm_objects_contract = SERVICE_OBMM_OBJECTS_H.read_text()
        qwen3_records = SERVICE_QWEN3_RECORDS_C.read_text()
        qwen3_records_contract = SERVICE_QWEN3_RECORDS_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_obmm_objects.inc"', source)
        self.assertIn('#include "mem_service_obmm_objects.h"', source)
        self.assertIn('#include "mem_service_obmm_objects.h"', obmm_objects)
        self.assertIn('#include "mem_service_guest_runtime.h"', obmm_objects)
        self.assertIn('#include "mem_service_object_contract.h"', obmm_objects)
        self.assertNotIn('#include "mem_service_internal.h"', obmm_objects)
        self.assertIn('#include "mem_service.h"', obmm_objects_contract)
        self.assertIn("struct mem_service_cluster_runtime;", obmm_objects_contract)
        self.assertIn("mem_service_fill_obmm_object_payload", obmm_objects)
        self.assertIn("mem_service_object_kind_name", obmm_objects)
        self.assertIn("mem_service_payload_arena_alloc", obmm_objects)
        self.assertIn("mem_service_put_obmm_object_record", obmm_objects)
        self.assertIn("OBMM object payload generation", readme)
        self.assertIn("runtime-adjacent translation unit", readme)
        self.assertIn("private OBMM object helper contract", readme)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", qwen3_records)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", qwen3_records_contract)
        self.assertNotIn(
            "static struct mem_service_record *mem_service_recycle_qwen3_runtime_record",
            qwen3_records,
        )
        self.assertFalse((SERVICE_DIR / "mem_service_obmm_objects.inc").exists())
        self.assertNotRegex(
            source,
            r"static int mem_service_payload_arena_alloc"
            r"\(struct mem_service_cluster_runtime \*rt,",
        )

    def test_prefix_kv_metadata_state_machine_is_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        metadata = SERVICE_METADATA_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_metadata.inc"', source)
        self.assertIn('#include "mem_service.h"', metadata)
        self.assertIn("#include <stdbool.h>", metadata)
        self.assertIn("#include <stdio.h>", metadata)
        self.assertIn("#include <string.h>", metadata)
        self.assertIn("mem_service_bootstrap_kvcache", metadata)
        self.assertIn("mem_service_apply_block_result", metadata)
        self.assertIn("mem_service_rebind_block_view", metadata)
        self.assertIn("mem_service_handoff_block_owner", metadata)
        self.assertIn("prefix/KV metadata state machine", readme)
        self.assertIn("standalone core translation unit", readme)
        self.assertIn("public service contract plus record helpers", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_metadata.inc").exists())
        self.assertNotIn("mem_service_internal.h", metadata)
        self.assertNotIn("mem_service_guest_runtime.h", metadata)
        self.assertNotRegex(
            source,
            r"int mem_service_bootstrap_kvcache"
            r"\(struct mem_service \*svc,",
        )

    def test_cluster_payload_publish_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_observe = SERVICE_CLUSTER_OBSERVE_C.read_text()
        cluster_payload = SERVICE_CLUSTER_PAYLOAD_C.read_text()
        cluster_payload_contract = SERVICE_CLUSTER_PAYLOAD_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_payload.inc"', source)
        self.assertIn('#include "mem_service_cluster_payload.h"', source)
        self.assertIn('#include "mem_service_cluster_payload.h"', cluster_payload)
        self.assertIn("mem_service_snapshot_metadata_records", cluster_payload)
        self.assertIn("mem_service_build_compact_summary", cluster_payload)
        self.assertIn("mem_service_write_cluster_payload", cluster_payload)
        self.assertIn("mem_service_build_compact_summary", cluster_payload_contract)
        self.assertIn("mem_service_write_cluster_payload", cluster_payload_contract)
        self.assertIn("struct mem_service_cluster_runtime *rt", cluster_payload_contract)
        self.assertIn("mem_service_write_cluster_payload(rt, svc", cluster_observe)
        self.assertIn("standalone guest runtime translation unit", readme)
        self.assertIn("private cluster payload helper\n  contract", readme)
        self.assertIn("cluster metadata payload", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_payload.inc").exists())
        self.assertNotRegex(
            source,
            r"static int mem_service_write_cluster_payload"
            r"\(struct mem_service \*svc,",
        )

    def test_cluster_payload_read_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_read = SERVICE_CLUSTER_READ_C.read_text()
        cluster_read_contract = SERVICE_CLUSTER_READ_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_read.inc"', source)
        self.assertIn('#include "mem_service_cluster_read.h"', source)
        self.assertIn('#include "mem_service_cluster_read.h"', cluster_read)
        self.assertIn("mem_service_try_read_stable_payload_region", cluster_read)
        self.assertIn("mem_service_wait_compact_summary_region_at_least", cluster_read)
        self.assertIn("mem_service_slot_find_record", cluster_read)
        self.assertIn("mem_service_try_read_stable_payload_region", cluster_read_contract)
        self.assertIn("mem_service_wait_compact_summary_region_at_least", cluster_read_contract)
        self.assertIn("mem_service_slot_find_record", cluster_read_contract)
        self.assertIn("stable cluster payload read", readme)
        self.assertIn("standalone guest\n  runtime read-side translation unit", readme)
        self.assertIn("private cluster read helper\n  contract", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_read.inc").exists())
        self.assertNotRegex(
            source,
            r"static bool mem_service_try_read_stable_payload_region"
            r"\(const struct mem_service_cluster_slot \*slot,",
        )

    def test_cluster_env_region_utils_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_utils = SERVICE_CLUSTER_UTILS_C.read_text()
        cluster_utils_contract = SERVICE_CLUSTER_UTILS_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_utils.inc"', source)
        self.assertIn('#include "mem_service_cluster_utils.h"', source)
        self.assertIn('#include "mem_service_cluster_utils.h"', cluster_utils)
        self.assertIn("mem_service_resolve_cluster_nodes", cluster_utils)
        self.assertIn("mem_service_update_region_range_at", cluster_utils)
        self.assertIn("mem_service_sync_remote_range", cluster_utils)
        self.assertIn("mem_service_resolve_cluster_nodes", cluster_utils_contract)
        self.assertIn("mem_service_update_region_range_at", cluster_utils_contract)
        self.assertIn("mem_service_sync_remote_range", cluster_utils_contract)
        self.assertIn("standalone guest runtime utility translation unit", readme)
        self.assertIn("private guest cluster utility\n  contract", readme)
        self.assertIn("cluster environment parsing", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_utils.inc").exists())
        self.assertNotRegex(
            source,
            r"static bool mem_service_resolve_cluster_nodes"
            r"\(char local_ip",
        )

    def test_cluster_bootstrap_runtime_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_runtime = SERVICE_CLUSTER_RUNTIME_C.read_text()
        cluster_runtime_contract = SERVICE_CLUSTER_RUNTIME_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_runtime.inc"', source)
        self.assertIn('#include "mem_service_cluster_runtime.h"', source)
        self.assertIn('#include "mem_service_cluster_runtime.h"', cluster_runtime)
        self.assertIn("mem_service_init_export_layout", cluster_runtime)
        self.assertIn("mem_service_activate_remote_slot", cluster_runtime)
        self.assertIn("mem_service_cluster_runtime_init", cluster_runtime)
        self.assertIn("mem_service_cluster_runtime_init", cluster_runtime_contract)
        self.assertIn("mem_service_cluster_runtime_require", cluster_runtime_contract)
        self.assertIn("mem_service_activate_remote_slot", cluster_runtime_contract)
        self.assertIn("guest OBMM cluster bootstrap", readme)
        self.assertIn("standalone transport runtime translation unit", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_runtime.inc").exists())
        self.assertNotRegex(
            source,
            r"static int mem_service_cluster_runtime_init"
            r"\(struct mem_service_cluster_runtime \*rt\)",
        )

    def test_cluster_queue_descriptor_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_queue = SERVICE_CLUSTER_QUEUE_C.read_text()
        cluster_queue_contract = SERVICE_CLUSTER_QUEUE_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_queue.inc"', source)
        self.assertIn('#include "mem_service_cluster_queue.h"', source)
        self.assertIn('#include "mem_service_cluster_queue.h"', cluster_queue)
        self.assertIn("mem_service_queue_barrier", cluster_queue)
        self.assertIn("mem_service_push_obmm_object_descs", cluster_queue)
        self.assertIn("mem_service_wait_remote_obmm_object_descs", cluster_queue)
        self.assertIn("mem_service_runtime_range_input_desc_matches", cluster_queue)
        self.assertIn("mem_service_queue_barrier", cluster_queue_contract)
        self.assertIn("mem_service_push_obmm_object_descs", cluster_queue_contract)
        self.assertIn("mem_service_wait_remote_obmm_object_descs", cluster_queue_contract)
        self.assertIn("mem_service_runtime_range_input_desc_matches", cluster_queue_contract)
        self.assertIn("guest OBMM SPSC queue barriers", readme)
        self.assertIn("standalone transport queue translation unit", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_queue.inc").exists())
        self.assertNotRegex(
            source,
            r"static int mem_service_queue_barrier"
            r"\(struct mem_service_cluster_runtime \*rt,",
        )

    def test_cluster_observe_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_observe = SERVICE_CLUSTER_OBSERVE_C.read_text()
        cluster_observe_contract = SERVICE_CLUSTER_OBSERVE_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_cluster_observe.inc"', source)
        self.assertIn('#include "mem_service_cluster_observe.h"', source)
        self.assertIn('#include "mem_service_cluster_observe.h"', cluster_observe)
        self.assertIn("mem_service_cluster_fetch_record", cluster_observe)
        self.assertIn("mem_service_publish_observe_cluster", cluster_observe)
        self.assertIn("mem_service_obmm_service_v0_ensure_cluster_runtime", cluster_observe)
        self.assertIn("mem_service_cluster_runtime_current", cluster_observe)
        self.assertIn("mem_service_cluster_fetch_record", cluster_observe_contract)
        self.assertIn("mem_service_publish_observe_cluster", cluster_observe_contract)
        self.assertIn("mem_service_obmm_service_v0_ensure_cluster_runtime", cluster_observe_contract)
        self.assertIn("cluster metadata fetch, observe", readme)
        self.assertIn("standalone transport observe translation unit", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_cluster_observe.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_publish_observe_cluster"
            r"\(struct mem_service \*svc,",
        )
        self.assertNotRegex(
            source,
            r"int mem_service_cluster_fetch_record"
            r"\(struct mem_service \*svc,",
        )

    def test_obmm_object_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        object_flow = SERVICE_OBMM_OBJECT_FLOW_C.read_text()
        object_flow_contract = SERVICE_OBMM_OBJECT_FLOW_H.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_obmm_object_flow.inc"', source)
        self.assertIn('#include "mem_service_obmm_object_flow.h"', source)
        self.assertIn('#include "mem_service_obmm_object_flow.h"', object_flow)
        self.assertIn("mem_service_obmm_service_v0_publish_resolve", object_flow)
        self.assertIn("obmm_service_v0_object_desc_put", object_flow)
        self.assertIn("obmm_service_v0_object_desc_get", object_flow)
        self.assertIn("qwen3_range_forward_handoff", object_flow)
        self.assertIn("mem_service_cluster_runtime_current", object_flow)
        self.assertIn("mem_service_obmm_service_v0_publish_resolve", object_flow_contract)
        self.assertIn("guest OBMM object publish", readme)
        self.assertIn("standalone transport object-flow translation unit", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_obmm_object_flow.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_resolve"
            r"\(struct mem_service \*svc,",
        )

    def test_qwen3_runtime_helpers_are_split_from_service_core(self):
        source = SERVICE_C.read_text()
        qwen3_runtime = SERVICE_QWEN3_RUNTIME_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_runtime.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', qwen3_runtime)
        self.assertIn("mem_service_qwen3_hidden_payload_checksum", qwen3_runtime)
        self.assertIn("mem_service_qwen3_kv_state_alloc", qwen3_runtime)
        self.assertIn("mem_service_qwen3_engram_candidates_key", qwen3_runtime)
        self.assertIn("mem_service_publish_qwen3_layer_range_placements", qwen3_runtime)
        self.assertIn("Qwen3 runtime payload checksum", readme)
        self.assertIn("standalone model helper", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_runtime.inc").exists())
        self.assertNotRegex(
            source,
            r"static uint64_t mem_service_qwen3_hidden_payload_checksum"
            r"\(const uint8_t \*bytes,",
        )
        self.assertNotRegex(
            source,
            r"static int mem_service_qwen3_kv_state_block_span"
            r"\(uint64_t payload_len,",
        )

    def test_qwen3_decode_barrier_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        decode_barrier = SERVICE_QWEN3_DECODE_BARRIER_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_decode_barrier.inc"', source)
        self.assertIn('#include "mem_service_cluster_runtime.h"', source)
        self.assertIn('#include "mem_service_qwen3_runtime.h"', source)
        self.assertIn('#include "mem_service_cluster_runtime.h"', decode_barrier)
        self.assertIn('#include "mem_service_qwen3_runtime.h"', decode_barrier)
        self.assertIn("mem_service_obmm_service_v0_publish_decode_round_done", decode_barrier)
        self.assertIn("mem_service_obmm_service_v0_wait_all_decode_round_done", decode_barrier)
        self.assertIn("qwen3_decode_round_barrier", decode_barrier)
        self.assertIn("Qwen3 decode-round publish", readme)
        self.assertIn("standalone model data-flow", readme)
        self.assertIn("translation unit", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_decode_barrier.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_decode_round_done"
            r"\(struct mem_service \*svc,",
        )

    def test_qwen3_runtime_range_wait_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        range_wait_flow = SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn(
            '#include "mem_service_qwen3_runtime_range_wait_flow.inc"',
            source,
        )
        self.assertIn('#include "mem_service_internal.h"', range_wait_flow)
        self.assertIn(
            "mem_service_obmm_service_v0_wait_runtime_range_input_view_internal",
            range_wait_flow,
        )
        self.assertIn(
            "mem_service_obmm_service_v0_wait_scheduler_work_item",
            range_wait_flow,
        )
        self.assertIn(
            "mem_service_obmm_service_v0_wait_runtime_range_input",
            range_wait_flow,
        )
        self.assertIn("mem_service_cluster_runtime_current", range_wait_flow)
        self.assertIn("Qwen3 runtime range", readme)
        self.assertIn("scheduler work-item resolution", readme)
        self.assertIn("standalone model data-flow", readme)
        self.assertFalse(
            (SERVICE_DIR / "mem_service_qwen3_runtime_range_wait_flow.inc").exists()
        )
        self.assertNotRegex(
            source,
            r"static int mem_service_obmm_service_v0_wait_runtime_range_input_view_internal"
            r"\s*\(",
        )
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_wait_scheduler_work_item"
            r"\s*\(",
        )

    def test_qwen3_runtime_range_publish_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        range_publish_flow = SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_runtime_range_publish_flow.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', range_publish_flow)
        self.assertIn(
            "mem_service_obmm_service_v0_publish_runtime_range_output",
            range_publish_flow,
        )
        self.assertIn("mem_service_qwen3_kv_state_alloc", range_publish_flow)
        self.assertIn("mem_service_push_obmm_object_desc_to", range_publish_flow)
        self.assertIn("mem_service_cluster_runtime_current", range_publish_flow)
        self.assertIn("Qwen3 runtime", readme)
        self.assertIn("range output", readme)
        self.assertIn("KV-state object publication", readme)
        self.assertIn("standalone model", readme)
        self.assertFalse(
            (SERVICE_DIR / "mem_service_qwen3_runtime_range_publish_flow.inc").exists()
        )
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_runtime_range_output"
            r"\s*\(\s*struct mem_service \*svc,",
        )

    def test_qwen3_kv_state_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        kv_state_flow = SERVICE_QWEN3_KV_STATE_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_kv_state_flow.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', kv_state_flow)
        self.assertIn("mem_service_obmm_service_v0_publish_runtime_range_kv_state", kv_state_flow)
        self.assertIn("mem_service_obmm_service_v0_try_resolve_range_kv_state_view", kv_state_flow)
        self.assertIn("mem_service_obmm_service_v0_resolve_previous_range_kv_state", kv_state_flow)
        self.assertIn("mem_service_cluster_runtime_current", kv_state_flow)
        self.assertIn("Qwen3 runtime range KV-state", readme)
        self.assertIn("standalone model", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_kv_state_flow.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_runtime_range_kv_state"
            r"\s*\(\s*struct mem_service \*svc,",
        )

    def test_qwen3_terminal_token_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        terminal_token_flow = SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_terminal_token_flow.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', terminal_token_flow)
        self.assertIn(
            "mem_service_obmm_service_v0_publish_terminal_token_result",
            terminal_token_flow,
        )
        self.assertIn(
            "mem_service_obmm_service_v0_publish_shortpath_terminal_token_result",
            terminal_token_flow,
        )
        self.assertIn("mem_service_obmm_service_v0_wait_terminal_token_result", terminal_token_flow)
        self.assertIn("mem_service_cluster_runtime_current", terminal_token_flow)
        self.assertIn("Qwen3 terminal token", readme)
        self.assertIn("standalone model", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_terminal_token_flow.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_terminal_token_result"
            r"\s*\(\s*struct mem_service \*svc,",
        )
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_wait_terminal_token_result"
            r"\s*\(\s*struct mem_service \*svc,",
        )

    def test_qwen3_engram_publish_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        engram_publish_flow = SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_engram_publish_flow.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', engram_publish_flow)
        self.assertIn("mem_service_pack_qwen3_engram_candidates", engram_publish_flow)
        self.assertIn(
            "mem_service_obmm_service_v0_publish_engram_candidates",
            engram_publish_flow,
        )
        self.assertIn("mem_service_obmm_service_v0_publish_engram_step", engram_publish_flow)
        self.assertIn("mem_service_cluster_runtime_current", engram_publish_flow)
        self.assertIn("mem_service_qwen3_engram_owner_index", engram_publish_flow)
        self.assertIn("Qwen3 engram candidate", readme)
        self.assertIn("standalone model", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_engram_publish_flow.inc").exists())
        self.assertNotRegex(
            source,
            r"static uint64_t mem_service_pack_qwen3_engram_candidates"
            r"\s*\(\s*uint64_t decode_step,",
        )
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_engram_step"
            r"\s*\(\s*struct mem_service \*svc,",
        )

    def test_qwen3_engram_wait_flow_is_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        engram_wait_flow = SERVICE_QWEN3_ENGRAM_WAIT_FLOW_C.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertNotIn('#include "mem_service_qwen3_engram_wait_flow.inc"', source)
        self.assertIn('#include "mem_service_internal.h"', engram_wait_flow)
        self.assertIn("mem_service_obmm_service_v0_wait_engram_candidates", engram_wait_flow)
        self.assertIn("mem_service_obmm_service_v0_wait_engram_selected_token", engram_wait_flow)
        self.assertIn("mem_service_obmm_service_v0_wait_engram_history", engram_wait_flow)
        self.assertIn("mem_service_obmm_service_v0_wait_engram_state", engram_wait_flow)
        self.assertIn("mem_service_cluster_runtime_current", engram_wait_flow)
        self.assertIn("mem_service_take_pending_qwen3_object_desc", engram_wait_flow)
        self.assertIn("Qwen3 engram candidate", readme)
        self.assertIn("selected-token", readme)
        self.assertIn("standalone", readme)
        self.assertFalse((SERVICE_DIR / "mem_service_qwen3_engram_wait_flow.inc").exists())
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_wait_engram_candidates"
            r"\s*\(\s*struct mem_service \*svc,",
        )
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_wait_engram_state"
            r"\s*\(\s*struct mem_service \*svc,",
        )

    def test_mem_service_uses_neutral_run_id_env_with_w5_compatibility(self):
        source = SERVICE_C.read_text()
        runtime_config = SERVICE_RUNTIME_CONFIG_H.read_text()
        range_publish_flow = SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_C.read_text()

        self.assertIn('#include "mem_service_internal.h"', source)
        self.assertIn("mem_service_run_id_from_env", runtime_config)
        self.assertIn('"MEM_SERVICE_RUN_ID"', runtime_config)
        self.assertIn('"SIM_W5_RUN_ID"', runtime_config)
        self.assertIn(
            "const char *service_run_id = mem_service_run_id_from_env();",
            range_publish_flow,
        )
        self.assertNotIn("w5_run_id", source)

    def test_qwen3_runtime_api_is_exposed_by_qwen3_adapter_header(self):
        generic_header = SERVICE_H.read_text()
        qwen3_header = SERVICE_QWEN3_H.read_text()
        guest_source = GUEST_C.read_text()

        self.assertIn('#include "components/mem_service/mem_service_qwen3.h"', guest_source)
        self.assertNotIn("mem_service_obmm_service_v0_wait_runtime_range_input", generic_header)
        self.assertNotIn("mem_service_obmm_service_v0_publish_runtime_range_output", generic_header)
        self.assertNotIn("mem_service_obmm_service_v0_wait_engram", generic_header)
        self.assertNotIn("MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT", generic_header)
        self.assertIn("mem_service_obmm_service_v0_wait_runtime_range_input", qwen3_header)
        self.assertIn("mem_service_obmm_service_v0_publish_runtime_range_output", qwen3_header)
        self.assertIn("mem_service_obmm_service_v0_wait_engram", qwen3_header)
        self.assertIn("MEM_SERVICE_OBMM_KIND_QWEN3_TOKEN_RESULT", qwen3_header)

    def test_llm_infer_internal_memory_symbols_are_not_w5_named(self):
        source = GUEST_C.read_text()

        self.assertNotIn("W4_QWEN3_W5_", source)
        self.assertNotIn("parse_qwen3_w5_", source)
        self.assertNotIn("qwen3_read_w5_", source)
        self.assertNotIn("qwen3_w5_memory_service_lookup_boundary", source)
        self.assertIn("QWEN3_MEMORY_SHORTPATH_STREAM_MAX", source)
        self.assertIn("parse_qwen3_memory_decision_config", source)
        self.assertIn("qwen3_memory_service_lookup_boundary", source)

    def test_qwen3_kv_state_uses_tiered_block_spans(self):
        source = (
            SERVICE_OBJECT_CONTRACT_H.read_text()
            + "\n"
            + SERVICE_INTERNAL_H.read_text()
            + "\n"
            + SERVICE_C.read_text()
            + "\n"
            + SERVICE_QWEN3_RUNTIME_C.read_text()
        )

        tier_names = [
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER0_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER1_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER2_BYTES",
            "MEM_SERVICE_OBMM_QWEN3_KV_STATE_BLOCK_TIER3_BYTES",
        ]
        tier_values = []

        for tier in tier_names:
            self.assertIn(tier, source)
            match = re.search(rf"#define {tier}\s+0x([0-9a-fA-F]+)ULL", source)
            if match:
                tier_values.append(int(match.group(1), 16))

        slot_bytes = re.search(
            r"#define MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES\s+0x([0-9a-fA-F]+)ULL",
            source,
        )
        self.assertIsNotNone(slot_bytes)
        tier_values.append(int(slot_bytes.group(1), 16))

        max_block_bytes = max(tier_values)
        over_max_payload_bytes = max_block_bytes + 1
        self.assertEqual(
            (over_max_payload_bytes + max_block_bytes - 1) // max_block_bytes,
            2,
        )
        self.assertIn("mem_service_qwen3_kv_state_block_span", source)
        self.assertIn("mem_service_qwen3_kv_state_alloc", source)
        self.assertIn("block_count =", source)
        self.assertIn("reserved_bytes = block_count * block_bytes", source)
        self.assertNotIn("kv_payload_len > MEM_SERVICE_OBMM_QWEN3_KV_STATE_SLOT_BYTES", source)

    def test_obmm_service_object_bytes_are_not_demo_named(self):
        source = SERVICE_OBJECT_CONTRACT_H.read_text()

        self.assertIn("MEM_SERVICE_OBMM_SERVICE_OBJECT_BYTES", source)
        self.assertNotIn("MEM_SERVICE_OBMM_DEMO_OBJECT_BYTES", source)

    def test_qwen3_guest_runtime_kv_payload_grows_past_fixed_guard(self):
        source = GUEST_C.read_text()

        self.assertNotIn("W4_QWEN3_MAX_KV_PAYLOAD_BYTES", source)
        self.assertNotIn("qwen3 range kv payload too large", source)
        self.assertIn("uint8_t *kv_payload;", source)
        self.assertIn("kv_payload_capacity", source)
        self.assertIn("qwen3_range_runtime_forward_reserve_kv", source)
        self.assertIn("qwen3 range kv payload reserve failed", source)

    def test_w4_guest_legacy_kvcache_payload_is_not_demo_named(self):
        sim_uapi_source = SIM_UAPI_RS.read_text()
        sources = [
            GUEST_C.read_text(),
            FOUR_NODE_W4_RUNNER.read_text(),
            EIGHT_NODE_W4_RUNNER.read_text(),
            sim_uapi_source,
        ]
        combined = "\n".join(sources)

        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertIn("W4_LEGACY_KVCACHE_PAYLOAD_BYTES", sim_uapi_source)
        self.assertIn("legacy_kvcache_payload", combined)
        self.assertNotIn("W4_DEMO_KVCACHE_PAYLOAD_BYTES", combined)
        self.assertNotIn("invalid_demo_kvcache_payload_bytes", combined)
        self.assertNotIn("legacy_demo_payload", combined)


if __name__ == "__main__":
    unittest.main()
