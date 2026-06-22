import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REPO_ROOT = ROOT.parents[1]
SERVICE_DIR = ROOT / "components" / "mem_service"
SERVICE_C = SERVICE_DIR / "mem_service.c"
SERVICE_H = SERVICE_DIR / "mem_service.h"
SERVICE_QWEN3_H = SERVICE_DIR / "mem_service_qwen3.h"
SERVICE_RECORDS_INC = SERVICE_DIR / "mem_service_records.inc"
SERVICE_QWEN3_RECORDS_INC = SERVICE_DIR / "mem_service_qwen3_records.inc"
SERVICE_QWEN3_RUNTIME_INC = SERVICE_DIR / "mem_service_qwen3_runtime.inc"
SERVICE_KEYS_INC = SERVICE_DIR / "mem_service_keys.inc"
SERVICE_OBJECT_REFS_INC = SERVICE_DIR / "mem_service_object_refs.inc"
SERVICE_OBMM_OBJECTS_INC = SERVICE_DIR / "mem_service_obmm_objects.inc"
SERVICE_METADATA_INC = SERVICE_DIR / "mem_service_metadata.inc"
SERVICE_CLUSTER_PAYLOAD_INC = SERVICE_DIR / "mem_service_cluster_payload.inc"
SERVICE_CLUSTER_READ_INC = SERVICE_DIR / "mem_service_cluster_read.inc"
SERVICE_CLUSTER_UTILS_INC = SERVICE_DIR / "mem_service_cluster_utils.inc"
SERVICE_CLUSTER_RUNTIME_INC = SERVICE_DIR / "mem_service_cluster_runtime.inc"
SERVICE_CLUSTER_QUEUE_INC = SERVICE_DIR / "mem_service_cluster_queue.inc"
SERVICE_CLUSTER_OBSERVE_INC = SERVICE_DIR / "mem_service_cluster_observe.inc"
SERVICE_OBMM_OBJECT_FLOW_INC = SERVICE_DIR / "mem_service_obmm_object_flow.inc"
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

        self.assertIn("Components do not install guest binaries directly", components_readme)
        self.assertIn(
            'MEM_SERVICE_SRC="$ROOT_DIR/components/mem_service/mem_service.c"',
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
            '"$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"',
            build_script,
        )
        self.assertIn("linqu_mem_service", build_script)
        self.assertIn("linqu_mem_service", run_app)
        self.assertIn("linqu_mem_service=1", run_app)
        self.assertIn("run_binary \"linqu_mem_service\" /bin/linqu_mem_service --smoke", run_app)
        self.assertTrue((CLI_DIR / "mem_service.c").exists())
        self.assertTrue((CLI_DIR / "Makefile").exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.c").exists())
        self.assertTrue((SERVICE_DIR / "mem_service_qwen3.h").exists())
        self.assertFalse((ROOT / "apps" / "mem_service_demo").exists())

    def test_record_caps_support_long_decode_runs(self):
        header = SERVICE_H.read_text()
        source = SERVICE_C.read_text()

        max_records = re.search(r"#define MEM_SERVICE_MAX_RECORDS\s+(\d+)U", header)
        cluster_records = re.search(r"#define MEM_SERVICE_CLUSTER_MAX_RECORDS\s+(\d+)", source)

        self.assertIsNotNone(max_records)
        self.assertIsNotNone(cluster_records)
        self.assertGreaterEqual(int(max_records.group(1)), 1024)
        self.assertGreaterEqual(int(cluster_records.group(1)), 1024)

    def test_full_record_table_recycles_old_qwen3_runtime_records(self):
        source = (
            SERVICE_C.read_text()
            + "\n"
            + SERVICE_QWEN3_RECORDS_INC.read_text()
            + "\n"
            + SERVICE_OBMM_OBJECTS_INC.read_text()
        )

        self.assertIn("MEM_SERVICE_QWEN3_RECORD_RETAIN_STEPS", source)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", source)
        self.assertIn('strstr(key, "decode-step")', source)
        self.assertIn('strstr(key, "/step/")', source)
        self.assertIn("rec = mem_service_alloc_record(svc);", source)
        self.assertIn("rec = mem_service_recycle_qwen3_runtime_record(svc, key);", source)

    def test_record_table_helpers_are_split_from_main_service_translation_unit(self):
        source = SERVICE_C.read_text()
        records = SERVICE_RECORDS_INC.read_text()
        qwen3_records = SERVICE_QWEN3_RECORDS_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_records.inc"', source)
        self.assertIn('#include "mem_service_qwen3_records.inc"', source)
        self.assertIn("mem_service_alloc_record", records)
        self.assertIn("mem_service_find_record", records)
        self.assertIn("mem_service_recycle_qwen3_runtime_record", qwen3_records)
        self.assertIn("mem_service_qwen3_key_decode_step", qwen3_records)
        self.assertNotIn("mem_service_recycle_qwen3_runtime_record", records)
        self.assertIn("Qwen3 streaming runtime record", readme)
        self.assertNotRegex(
            source,
            r"static struct mem_service_record \*mem_service_alloc_record"
            r"\(struct mem_service \*svc\)\s*\{",
        )

    def test_key_construction_helpers_are_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        keys = SERVICE_KEYS_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_keys.inc"', source)
        self.assertIn("mem_service_build_two_part_key", keys)
        self.assertIn("mem_service_build_prefix_key_from_parts_checked", keys)
        self.assertIn("mem_service_build_block_key_from_hash_checked", keys)
        self.assertIn("Productization Split Contract", readme)
        self.assertIn("guest component and as a host-side service", readme)
        self.assertNotRegex(
            source,
            r"static int mem_service_build_two_part_key"
            r"\(const char \*prefix,",
        )

    def test_object_ref_helpers_are_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        object_refs = SERVICE_OBJECT_REFS_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_object_refs.inc"', source)
        self.assertIn("mem_service_checksum_bytes", object_refs)
        self.assertIn("mem_service_record_to_lingqu_obmm_ref", object_refs)
        self.assertIn("object-reference projection", readme)
        self.assertNotRegex(
            source,
            r"int mem_service_record_to_lingqu_obmm_ref"
            r"\(const struct mem_service_record \*record,",
        )

    def test_obmm_object_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        obmm_objects = SERVICE_OBMM_OBJECTS_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_obmm_objects.inc"', source)
        self.assertIn("mem_service_fill_obmm_object_payload", obmm_objects)
        self.assertIn("mem_service_object_kind_name", obmm_objects)
        self.assertIn("mem_service_payload_arena_alloc", obmm_objects)
        self.assertIn("mem_service_put_obmm_object_record", obmm_objects)
        self.assertIn("OBMM object payload generation", readme)
        self.assertNotRegex(
            source,
            r"static int mem_service_payload_arena_alloc"
            r"\(struct mem_service_cluster_runtime \*rt,",
        )

    def test_prefix_kv_metadata_state_machine_is_split_for_host_guest_core_reuse(self):
        source = SERVICE_C.read_text()
        metadata = SERVICE_METADATA_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_metadata.inc"', source)
        self.assertIn("mem_service_bootstrap_kvcache", metadata)
        self.assertIn("mem_service_apply_block_result", metadata)
        self.assertIn("mem_service_rebind_block_view", metadata)
        self.assertIn("mem_service_handoff_block_owner", metadata)
        self.assertIn("prefix/KV metadata state machine", readme)
        self.assertNotRegex(
            source,
            r"int mem_service_bootstrap_kvcache"
            r"\(struct mem_service \*svc,",
        )

    def test_cluster_payload_publish_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_payload = SERVICE_CLUSTER_PAYLOAD_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_payload.inc"', source)
        self.assertIn("mem_service_snapshot_metadata_records", cluster_payload)
        self.assertIn("mem_service_build_compact_summary", cluster_payload)
        self.assertIn("mem_service_write_cluster_payload", cluster_payload)
        self.assertIn("cluster metadata payload", readme)
        self.assertNotRegex(
            source,
            r"static int mem_service_write_cluster_payload"
            r"\(struct mem_service \*svc,",
        )

    def test_cluster_payload_read_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_read = SERVICE_CLUSTER_READ_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_read.inc"', source)
        self.assertIn("mem_service_try_read_stable_payload_region", cluster_read)
        self.assertIn("mem_service_wait_compact_summary_region_at_least", cluster_read)
        self.assertIn("mem_service_slot_find_record", cluster_read)
        self.assertIn("stable cluster payload read", readme)
        self.assertNotRegex(
            source,
            r"static bool mem_service_try_read_stable_payload_region"
            r"\(const struct mem_service_cluster_slot \*slot,",
        )

    def test_cluster_env_region_utils_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_utils = SERVICE_CLUSTER_UTILS_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_utils.inc"', source)
        self.assertIn("mem_service_resolve_cluster_nodes", cluster_utils)
        self.assertIn("mem_service_update_region_range_at", cluster_utils)
        self.assertIn("mem_service_sync_remote_range", cluster_utils)
        self.assertIn("cluster environment parsing", readme)
        self.assertNotRegex(
            source,
            r"static bool mem_service_resolve_cluster_nodes"
            r"\(char local_ip",
        )

    def test_cluster_bootstrap_runtime_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_runtime = SERVICE_CLUSTER_RUNTIME_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_runtime.inc"', source)
        self.assertIn("mem_service_init_export_layout", cluster_runtime)
        self.assertIn("mem_service_activate_remote_slot", cluster_runtime)
        self.assertIn("mem_service_cluster_runtime_init", cluster_runtime)
        self.assertIn("guest OBMM cluster bootstrap", readme)
        self.assertNotRegex(
            source,
            r"static int mem_service_cluster_runtime_init"
            r"\(struct mem_service_cluster_runtime \*rt\)",
        )

    def test_cluster_queue_descriptor_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_queue = SERVICE_CLUSTER_QUEUE_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_queue.inc"', source)
        self.assertIn("mem_service_queue_barrier", cluster_queue)
        self.assertIn("mem_service_push_obmm_object_descs", cluster_queue)
        self.assertIn("mem_service_wait_remote_obmm_object_descs", cluster_queue)
        self.assertIn("guest OBMM SPSC queue barriers", readme)
        self.assertNotRegex(
            source,
            r"static int mem_service_queue_barrier"
            r"\(struct mem_service_cluster_runtime \*rt,",
        )

    def test_cluster_observe_helpers_are_split_from_runtime_main(self):
        source = SERVICE_C.read_text()
        cluster_observe = SERVICE_CLUSTER_OBSERVE_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_cluster_observe.inc"', source)
        self.assertIn("mem_service_cluster_fetch_record", cluster_observe)
        self.assertIn("mem_service_publish_observe_cluster", cluster_observe)
        self.assertIn("mem_service_obmm_service_v0_ensure_cluster_runtime", cluster_observe)
        self.assertIn("cluster metadata fetch, observe", readme)
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
        object_flow = SERVICE_OBMM_OBJECT_FLOW_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_obmm_object_flow.inc"', source)
        self.assertIn("mem_service_obmm_service_v0_publish_resolve", object_flow)
        self.assertIn("obmm_service_v0_object_desc_put", object_flow)
        self.assertIn("obmm_service_v0_object_desc_get", object_flow)
        self.assertIn("qwen3_range_forward_handoff", object_flow)
        self.assertIn("guest OBMM object publish", readme)
        self.assertNotRegex(
            source,
            r"int mem_service_obmm_service_v0_publish_resolve"
            r"\(struct mem_service \*svc,",
        )

    def test_qwen3_runtime_helpers_are_split_from_service_core(self):
        source = SERVICE_C.read_text()
        qwen3_runtime = SERVICE_QWEN3_RUNTIME_INC.read_text()
        readme = (SERVICE_DIR / "README.md").read_text()

        self.assertIn('#include "mem_service_qwen3_runtime.inc"', source)
        self.assertIn("mem_service_qwen3_hidden_payload_checksum", qwen3_runtime)
        self.assertIn("mem_service_qwen3_kv_state_alloc", qwen3_runtime)
        self.assertIn("mem_service_qwen3_engram_candidates_key", qwen3_runtime)
        self.assertIn("mem_service_publish_qwen3_layer_range_placements", qwen3_runtime)
        self.assertIn("Qwen3 runtime payload checksum", readme)
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

    def test_mem_service_uses_neutral_run_id_env_with_w5_compatibility(self):
        source = SERVICE_C.read_text()

        self.assertIn("mem_service_run_id_from_env", source)
        self.assertIn('"MEM_SERVICE_RUN_ID"', source)
        self.assertIn('"SIM_W5_RUN_ID"', source)
        self.assertIn("const char *service_run_id = mem_service_run_id_from_env();", source)
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
        source = SERVICE_C.read_text() + "\n" + SERVICE_QWEN3_RUNTIME_INC.read_text()

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
        source = SERVICE_C.read_text()

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
