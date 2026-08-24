#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
SIM_UAPI_W5_PROFILE="${SIM_UAPI_W5_PROFILE:-}"
SIM_W5_CLUSTER_NODE_COUNT="${SIM_W5_CLUSTER_NODE_COUNT:-8}"
TEE_BIN="${TEE_BIN:-/usr/bin/tee}"

case "$SIM_W5_CLUSTER_NODE_COUNT" in
  2)
    NODE_IDS=(nodeA nodeB)
    NODE_IPS=(10.0.0.1 10.0.0.2)
    DEFAULT_PORT_NUM=2
    ;;
  3)
    NODE_IDS=(nodeA nodeB nodeC)
    NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3)
    DEFAULT_PORT_NUM=2
    ;;
  4)
    NODE_IDS=(nodeA nodeB nodeC nodeD)
    NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)
    DEFAULT_PORT_NUM=3
    ;;
  8)
    NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
    NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
    DEFAULT_PORT_NUM=7
    ;;
  *)
    echo "SIM_W5_CLUSTER_NODE_COUNT must be 2, 3, 4, or 8: $SIM_W5_CLUSTER_NODE_COUNT" >&2
    exit 2
    ;;
esac
SIM_W5_GUEST_ENGINE="${SIM_W5_GUEST_ENGINE:-busybox}"
case "$SIM_W5_GUEST_ENGINE" in
  busybox|openEuler)
    ;;
  *)
    echo "SIM_W5_GUEST_ENGINE must be busybox or openEuler: $SIM_W5_GUEST_ENGINE" >&2
    exit 2
    ;;
esac
if [[ "$SIM_W5_GUEST_ENGINE" == "openEuler" ]]; then
  if [[ -z "${SIM_W5_OE_DISK_IMAGE:-}" ]]; then
    echo "openEuler guest engine requires SIM_W5_OE_DISK_IMAGE (openEuler qcow2 disk image)" >&2
    exit 2
  fi
  if [[ ! -f "$SIM_W5_OE_DISK_IMAGE" ]]; then
    echo "SIM_W5_OE_DISK_IMAGE not found: $SIM_W5_OE_DISK_IMAGE" >&2
    exit 2
  fi
fi
SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION="${SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION:-0}"

w5_profile_default_w4_backend() {
  case "$1" in
    ""|qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      echo qwen3_dense
      ;;
    deepseek_v4_flash_decode)
      echo deepseek-v4-flash
      ;;
    *)
      echo ""
      ;;
  esac
}

w5_profile_default_engram() {
  case "$1" in
    qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      echo 1
      ;;
    *)
      echo 0
      ;;
  esac
}

if [[ -n "$SIM_UAPI_W5_PROFILE" && -z "$(w5_profile_default_w4_backend "$SIM_UAPI_W5_PROFILE")" ]]; then
  echo "unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE" >&2
  exit 2
fi

if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w5_inference_cluster.trace.latest.txt}"
  RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE}_${RANDOM}}"
else
  TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w4_guest.trace.latest.txt}"
  RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w4guest8_${RANDOM}}"
fi
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w5_inference_cluster_summary.${RUN_ID_BASE}.txt}"
else
  RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w4_guest_summary.${RUN_ID_BASE}.txt}"
fi
RUN_INITRAMFS_DIR="$OUT_DIR/initramfs.${RUN_ID_BASE}"
RUN_INITRAMFS_IMAGE="$OUT_DIR/initramfs.${RUN_ID_BASE}.cpio.gz"
RUN_ENV_FILE=""
# Use a short unique suffix for the shared dir to stay under macOS 104-byte UNIX socket path limit.
_SHORT_SHARED_SUFFIX="$(printf '%s' "$RUN_ID_BASE" | cksum | cut -d' ' -f1)_${RANDOM}"
UB_FM_SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ubqe_${_SHORT_SHARED_SUFFIX}}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
APP_WAIT_SECS="${APP_WAIT_SECS:-600}"
if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  W4_GUEST_PROGRESS_INTERVAL_SECS="${SIM_W5_PROGRESS_INTERVAL_SECS:-${W4_GUEST_PROGRESS_INTERVAL_SECS:-180}}"
else
  W4_GUEST_PROGRESS_INTERVAL_SECS="${W4_GUEST_PROGRESS_INTERVAL_SECS:-180}"
fi
PROGRESS_INTERVAL_ENV_NAME="W4_GUEST_PROGRESS_INTERVAL_SECS"
if [[ -n "$SIM_UAPI_W5_PROFILE" && -n "${SIM_W5_PROGRESS_INTERVAL_SECS:-}" ]]; then
  PROGRESS_INTERVAL_ENV_NAME="SIM_W5_PROGRESS_INTERVAL_SECS"
fi
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
QEMU_MEM="${QEMU_MEM:-8G}"
PORT_NUM="${UB_SIM_PORT_NUM:-$DEFAULT_PORT_NUM}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST="${SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST:-/tmp/simpler-host-engram-context-artifacts/host_engram_context_manifest.json}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-$(w5_profile_default_w4_backend "$SIM_UAPI_W5_PROFILE")}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}"
SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
SIM_LLM_INFER_PROMPT_TOKEN_IDS="${SIM_LLM_INFER_PROMPT_TOKEN_IDS:-${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-81378,37585,374}}"
SIM_QWEN3_SAMPLER_TOP_K="${SIM_QWEN3_SAMPLER_TOP_K:-1}"
SIM_QWEN3_SAMPLER_TOP_P_MILLI="${SIM_QWEN3_SAMPLER_TOP_P_MILLI:-1000}"
SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="${SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI:-1000}"
SIM_QWEN3_SAMPLER_SEED="${SIM_QWEN3_SAMPLER_SEED:-0}"
SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-$(w5_profile_default_engram "$SIM_UAPI_W5_PROFILE")}"
SIM_QWEN3_GUEST_ENGRAM_MODE="${SIM_QWEN3_GUEST_ENGRAM_MODE:-cpu}"
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE="${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE:-$SIM_W5_CLUSTER_NODE_COUNT}"
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE="${SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE:-3}"
SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI="${SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI:-1000}"
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW="${SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW:-0}"
SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS="${SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS:-}"
SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION="${SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-disabled}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS:-}"
SIM_QWEN3_GUEST_ENGRAM_STATE_REF="${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF="${SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF:-}"
SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}"
SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}"
SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT"
SIM_W5_FLASH_WEIGHT_CATALOG="${SIM_W5_FLASH_WEIGHT_CATALOG:-${SIM_W5_TEST_FLASH_WEIGHT_CATALOG:-}}"
SIM_W5_FLASH_WEIGHT_CATALOG_GUEST="$SIM_W5_FLASH_WEIGHT_CATALOG"
SIM_W5_MEMORY_SERVICE="${SIM_W5_MEMORY_SERVICE:-}"
SIM_W5_SERVING_REQUEST_ID="${SIM_W5_SERVING_REQUEST_ID:-}"
SIM_W5_SERVING_REQUESTS_FILE="${SIM_W5_SERVING_REQUESTS_FILE:-}"
SIM_W5_SERVING_REQUESTS_FILE_GUEST="$SIM_W5_SERVING_REQUESTS_FILE"
SIM_W5_SERVING_REQUEST_COUNT="${SIM_W5_SERVING_REQUEST_COUNT:-}"
SIM_W5_SERVING_DECODE_STEPS_TOTAL="${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-}"
SIM_W5_SERVING_QUEUE="${SIM_W5_SERVING_QUEUE:-0}"
SIM_W5_SERVING_INGRESS="${SIM_W5_SERVING_INGRESS:-cluster}"
if [[ -n "$SIM_UAPI_W5_PROFILE" &&
      "$SIM_W5_SERVING_QUEUE" == "1" &&
      -z "$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR" &&
      -z "$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT" ]]; then
  SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$OUT_DIR/w5_serving_object_service_store.${RUN_ID_BASE}.json"
  SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT"
fi
SIM_W5_SERVING_SUBMIT_REQUESTS_FILE="${SIM_W5_SERVING_SUBMIT_REQUESTS_FILE:-}"
SIM_W5_SERVING_SUBMIT_WAIT_SECS="${SIM_W5_SERVING_SUBMIT_WAIT_SECS:-900}"
SIM_W5_MEMORY_STORE="${SIM_W5_MEMORY_STORE:-}"
SIM_W5_MEMORY_OBJECT_STORE="${SIM_W5_MEMORY_OBJECT_STORE:-}"
SIM_W5_MEMORY_ENGRAM_STATE="${SIM_W5_MEMORY_ENGRAM_STATE:-}"
SIM_W5_MEMORY_REGISTRY_DIR="${SIM_W5_MEMORY_REGISTRY_DIR:-}"
SIM_W5_TEST_MEMORY_DECISION_STORE="${SIM_W5_TEST_MEMORY_DECISION_STORE:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE="${SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE:-}"
SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND="${SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID="${SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID="${SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_ACTION="${SIM_W5_TEST_MEMORY_SHORTPATH_ACTION:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID="${SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START="${SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END="${SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND="${SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM="${SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF="${SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF:-}"
SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF="${SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF:-}"
SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT="${SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START="${SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END="${SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION="${SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM="${SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE="${SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE:-}"
SIM_W5_TEST_REQUIRE_PREFIX_CACHE="${SIM_W5_TEST_REQUIRE_PREFIX_CACHE:-0}"
SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT="${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_STREAM="${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH="${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH_GUEST="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH"
SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT="${SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH="${SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH:-}"
SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH_GUEST="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH"
SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID="${SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID:-}"
SIM_W5_TEST_MEMORY_PREFETCH_SCOPE="${SIM_W5_TEST_MEMORY_PREFETCH_SCOPE:-}"
SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX="${SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX:-}"
SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM="${SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM:-}"
SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS="${SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS:-}"
SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS="${SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS:-}"
SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS="${SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH:-}"
SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH_GUEST="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH"
SIM_W5_TEST_MEMORY_GSVA_KV="${SIM_W5_TEST_MEMORY_GSVA_KV:-}"
SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH="${SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH:-}"
SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="${SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS:-}"
SIM_QWEN3_RUNTIME_RANGE_WAIT_MS="${SIM_QWEN3_RUNTIME_RANGE_WAIT_MS:-}"
SIM_W4_UAPI_COMPLETION_TIMEOUT_MS="${SIM_W4_UAPI_COMPLETION_TIMEOUT_MS:-900000}"
SIM_W4_RESOURCE_ASSERTIONS="${SIM_W4_RESOURCE_ASSERTIONS:-0}"
FATAL_GUEST_PATTERN="rcu_preempt|RCU grace-period|self-detected stall|detected stalls on CPUs/tasks|rx msg plen invalid|poller rx msg failed, ret=-22|timeout waiting completions|qwen3 .*missing|qwen3 .*mismatch|dispatch payload mismatch|linqu_llm_infer failed|Kernel panic|\\[w4_guest\\] fail"
FATAL_QEMU_PATTERN="SIM_DEC: cpu read failed|ub_link write failed|bounded write timed out|rx msg plen invalid|poller rx msg failed"

ALL_IPS_CSV="${(j:,:)NODE_IPS}"

stage_qwen3_object_service_snapshot() {
  local snapshot_path="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT"
  local payload_index_src=""
  local payload_index_guest_json="/tmp/lingqu_object_service_snapshot.json"
  local payload_index_guest_bin="$RUN_INITRAMFS_DIR/tmp/lingqu_object_service_snapshot.bin"

  if [[ -z "$snapshot_path" || "$snapshot_path" == hex:* ]]; then
    return 0
  fi
  if [[ "$snapshot_path" == *.json ]]; then
    payload_index_src="${snapshot_path%.json}.bin"
  else
    payload_index_src="${snapshot_path}.bin"
  fi
  if [[ ! -f "$payload_index_src" ]]; then
    if [[ ! -e "$snapshot_path" ]] ||
       { [[ "$snapshot_path" == "$SIM_W5_MEMORY_OBJECT_STORE" &&
            "${SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED:-0}" == "1" ]] &&
         grep -q '"records": \[\]' "$snapshot_path"; }; then
      trace "prepare: model Object Service snapshot will be created by runtime path=$snapshot_path"
      return 0
    fi
    trace "FAIL: qwen3 Object Service payload index is missing source=$payload_index_src snapshot=$snapshot_path"
    return 1
  fi
  mkdir -p "$RUN_INITRAMFS_DIR/tmp"
  cp "$payload_index_src" "$payload_index_guest_bin"
  SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST="$payload_index_guest_json"
  trace "prepare: staged qwen3 Object Service payload index source=$payload_index_src guest=$payload_index_guest_bin"
}

stage_flash_weight_catalog() {
  local catalog_path="$SIM_W5_FLASH_WEIGHT_CATALOG"
  local catalog_guest_path="/tmp/deepseek_v4_flash_weight.catalog"
  local catalog_guest_file="$RUN_INITRAMFS_DIR$catalog_guest_path"

  if [[ -z "$catalog_path" ]]; then
    return 0
  fi
  if [[ ! -f "$catalog_path" ]]; then
    trace "FAIL: DeepSeek V4 Flash weight catalog is missing path=$catalog_path"
    return 1
  fi
  mkdir -p "$(dirname "$catalog_guest_file")"
  cp "$catalog_path" "$catalog_guest_file"
  SIM_W5_FLASH_WEIGHT_CATALOG_GUEST="$catalog_guest_path"
  trace "prepare: staged DeepSeek V4 Flash weight catalog source=$catalog_path guest=$catalog_guest_path"
}

stage_w5_memory_shortpath_stream() {
  local stream_path="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH"
  local stream_guest_path="/tmp/w5_memory_shortpath_stream.txt"
  local stream_guest_file="$RUN_INITRAMFS_DIR$stream_guest_path"

  if [[ -z "$stream_path" ]]; then
    return 0
  fi
  if [[ ! -f "$stream_path" ]]; then
    trace "FAIL: W5 Memory shortpath stream file is missing path=$stream_path"
    return 1
  fi
  mkdir -p "$(dirname "$stream_guest_file")"
  cp "$stream_path" "$stream_guest_file"
  SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH_GUEST="$stream_guest_path"
  trace "prepare: staged W5 Memory shortpath stream source=$stream_path guest=$stream_guest_path"
}

stage_w5_memory_shortpath_kv_stream() {
  local stream_path="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH"
  local stream_guest_path="/tmp/w5_memory_shortpath_kv_stream.txt"
  local stream_guest_file="$RUN_INITRAMFS_DIR$stream_guest_path"

  if [[ -z "$stream_path" ]]; then
    return 0
  fi
  if [[ ! -f "$stream_path" ]]; then
    trace "FAIL: W5 Memory shortpath KV stream file is missing path=$stream_path"
    return 1
  fi
  mkdir -p "$(dirname "$stream_guest_file")"
  cp "$stream_path" "$stream_guest_file"
  SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH_GUEST="$stream_guest_path"
  trace "prepare: staged W5 Memory shortpath KV stream source=$stream_path guest=$stream_guest_path"
}

stage_w5_memory_prefix_cache_kv_stream() {
  local stream_path="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH"
  local stream_guest_path="/tmp/w5_memory_prefix_cache_kv_stream.txt"
  local stream_guest_file="$RUN_INITRAMFS_DIR$stream_guest_path"

  if [[ -z "$stream_path" ]]; then
    return 0
  fi
  if [[ ! -f "$stream_path" ]]; then
    trace "FAIL: W5 Memory prefix-cache KV stream file is missing path=$stream_path"
    return 1
  fi
  mkdir -p "$(dirname "$stream_guest_file")"
  cp "$stream_path" "$stream_guest_file"
  SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH_GUEST="$stream_guest_path"
  trace "prepare: staged W5 Memory prefix-cache KV stream source=$stream_path guest=$stream_guest_path"
}

resolve_w5_serving_requests_config() {
  local requests_path="$SIM_W5_SERVING_REQUESTS_FILE"

  if [[ -z "$requests_path" ]]; then
    return 0
  fi
  if [[ ! -f "$requests_path" ]]; then
    trace "FAIL: W5 serving request file is missing path=$requests_path"
    return 1
  fi
  if ! "$SCRIPT_DIR/w5_serving_entry.py" --requests "$requests_path" --validate-only >/dev/null; then
    trace "FAIL: W5 serving request file is invalid path=$requests_path"
    return 1
  fi
  if [[ -z "$SIM_W5_SERVING_REQUEST_COUNT" ]]; then
    SIM_W5_SERVING_REQUEST_COUNT="$("$SCRIPT_DIR/w5_serving_entry.py" --requests "$requests_path" --print-request-count)"
  fi
  if [[ -z "$SIM_W5_SERVING_DECODE_STEPS_TOTAL" ]]; then
    SIM_W5_SERVING_DECODE_STEPS_TOTAL="$("$SCRIPT_DIR/w5_serving_entry.py" --requests "$requests_path" --print-total-decode-steps)"
  fi
  trace "prepare: W5 serving requests requests=$SIM_W5_SERVING_REQUEST_COUNT total_decode_steps=$SIM_W5_SERVING_DECODE_STEPS_TOTAL path=$requests_path"
}

stage_w5_serving_requests_file() {
  local requests_path="$SIM_W5_SERVING_REQUESTS_FILE"
  local requests_guest_path="/tmp/w5_serving_requests.txt"
  local requests_guest_file="$RUN_INITRAMFS_DIR$requests_guest_path"

  if [[ -z "$requests_path" ]]; then
    return 0
  fi
  resolve_w5_serving_requests_config || return 1
  mkdir -p "$(dirname "$requests_guest_file")"
  cp "$requests_path" "$requests_guest_file"
  SIM_W5_SERVING_REQUESTS_FILE_GUEST="$requests_guest_path"
  trace "prepare: staged W5 serving requests source=$requests_path guest=$requests_guest_path requests=$SIM_W5_SERVING_REQUEST_COUNT total_decode_steps=$SIM_W5_SERVING_DECODE_STEPS_TOTAL"
}

source "$SCRIPT_DIR/qemu_ub_common.sh"

append_kernel_arg_if_missing() {
  local arg="$1"
  local key="${arg%%=*}"

  if [[ "$APPEND_BASE" != *"$key="* ]]; then
    APPEND_BASE="${APPEND_BASE} ${arg}"
  fi
}

append_kernel_arg_if_missing "pmd_mapping=25%"
append_kernel_arg_if_missing "obmm.skip_cache_maintain=1"
case "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" in
  deepseek-v4-flash-simpler|deepseek_v4_flash_simpler|deepseek-v4-flash-official|deepseek_v4_flash_official)
    # The synchronous simulator backend intentionally holds the only guest
    # vCPU while a real simpler range executes. Operation deadlines still
    # detect a hung dispatch; an RCU wall-clock warning is not actionable here.
    append_kernel_arg_if_missing "rcupdate.rcu_cpu_stall_suppress=1"
    ;;
  *)
    append_kernel_arg_if_missing "rcupdate.rcu_cpu_stall_timeout=300"
    ;;
esac

trace() {
  local msg="$1"
  printf '[w4guest8] %s\n' "$msg" | "$TEE_BIN" -a "$TRACE_FILE" >&2
}

is_qwen3_dense_profile() {
  local profile="$1"
  [[ "$profile" == "qwen3_dense_reference" || "$profile" == "qwen3_dense" ]]
}

is_deepseek_v4_flash_profile() {
  local profile="$1"
  [[ "$profile" == "deepseek-v4-flash" ||
     "$profile" == "deepseek_v4_flash" ||
     "$profile" == "deepseek-v4-flash-simpler" ||
     "$profile" == "deepseek_v4_flash_simpler" ||
     "$profile" == "deepseek-v4-flash-official" ||
     "$profile" == "deepseek_v4_flash_official" ]]
}

is_model_range_profile() {
  is_qwen3_dense_profile "$1" || is_deepseek_v4_flash_profile "$1"
}

validate_w5_profile_runtime() {
  case "$SIM_UAPI_W5_PROFILE" in
    "")
      return 0
      ;;
    qwen3_0_6b_decode|qwen3_0_6b_engram_decode)
      if [[ "${SIM_QWEN3_DENSE_MODEL_KEY:-}" == "qwen3-14b" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires Qwen3-0.6B-compatible weights, got model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-unknown}"
        return 1
      fi
      ;;
    qwen3_14b_decode|qwen3_14b_engram_decode)
      if [[ "${SIM_QWEN3_DENSE_MODEL_KEY:-}" != "qwen3-14b" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires Qwen3-14B weights, got model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-unknown}"
        return 1
      fi
      ;;
    deepseek_v4_flash_decode)
      if ! is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires DeepSeek V4 Flash backend, got SIM_UAPI_W4_CHIPBACKEND_PROFILE=$SIM_UAPI_W4_CHIPBACKEND_PROFILE"
        return 1
      fi
      ;;
    *)
      trace "FAIL: unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE"
      return 1
      ;;
  esac

  case "$SIM_UAPI_W5_PROFILE" in
    qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" != "1" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires SIM_QWEN3_GUEST_ENGRAM=1"
        return 1
      fi
      ;;
    qwen3_0_6b_decode|qwen3_14b_decode|deepseek_v4_flash_decode)
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires SIM_QWEN3_GUEST_ENGRAM=0"
        return 1
      fi
      ;;
  esac
  return 0
}

validate_qwen3_weights_path() {
  if ! is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    return 0
  fi
  local weights_path="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  local required

  if [[ -z "$weights_path" ]]; then
    trace "FAIL: $SIM_UAPI_W4_CHIPBACKEND_PROFILE requires SIM_QWEN3_DENSE_WEIGHTS_PATH"
    return 1
  fi
  if [[ ! -d "$weights_path" ]]; then
    trace "FAIL: qwen3 weights path is not a directory path=$weights_path"
    return 1
  fi
  for required in config.json tokenizer.json; do
    if [[ ! -f "$weights_path/$required" ]]; then
      trace "FAIL: qwen3 weights path missing $required path=$weights_path"
      return 1
    fi
  done
  if [[ ! -f "$weights_path/model.safetensors" && ! -f "$weights_path/model.safetensors.index.json" ]]; then
    trace "FAIL: qwen3 weights path missing model.safetensors or model.safetensors.index.json path=$weights_path"
    return 1
  fi
  trace "prepare: qwen3 weights path ok path=$weights_path"
  return 0
}

validate_qwen3_runtime_object_view_source() {
  return 0
}

wait_for_log_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]] && rg -q "$pattern" "$file"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

wait_for_log_pass_or_fail_since() {
  local file="$1"
  local start_line="$2"
  local pass_pattern="$3"
  local fail_pattern="$4"
  local timeout_s="$5"
  local pass_count="${6:-1}"
  local deadline=$((SECONDS + timeout_s))
  local tmp
  local count

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      count=0
      if [[ -n "$tmp" ]]; then
        count="$(printf '%s\n' "$tmp" | rg -c "$pass_pattern" || true)"
      fi
      if (( count >= pass_count )); then
        return 0
      fi
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$fail_pattern"; then
        return 1
      fi
    fi
    sleep 0.2
  done
  return 2
}

wait_for_all_logs_pass_or_fail_since() {
  local pass_pattern="$1"
  local fail_pattern="$2"
  local timeout_s="$3"
  local pass_count="${4:-1}"
  local deadline=$((SECONDS + timeout_s))
  local wait_start="$SECONDS"
  local progress_interval="$W4_GUEST_PROGRESS_INTERVAL_SECS"
  local next_progress=0
  local node_id guest_log start_line tmp count all_pass

  if [[ ! "$progress_interval" =~ '^[0-9]+$' ]]; then
    trace "progress: invalid ${PROGRESS_INTERVAL_ENV_NAME}=$progress_interval; using 180"
    progress_interval=180
  fi
  if (( progress_interval > 0 )); then
    trace "progress: reporting_interval_s=$progress_interval expected_decode_steps=$pass_count"
    next_progress=$((SECONDS + progress_interval))
  else
    trace "progress: reporting disabled ${PROGRESS_INTERVAL_ENV_NAME}=0"
  fi

  while (( SECONDS < deadline )); do
    all_pass=1
    for node_id in "${NODE_IDS[@]}"; do
      guest_log="$RUN_DIR/${node_id}_guest.log"
      start_line="${START_LINES[$node_id]}"
      count=0
      if [[ -f "$guest_log" ]]; then
        tmp="$(tail -n "+$((start_line + 1))" "$guest_log" 2>/dev/null || true)"
        if [[ -n "$tmp" ]]; then
          if printf '%s\n' "$tmp" | rg -q "$fail_pattern"; then
            trace "FAIL: w4 guest fatal marker on $node_id"
            printf '%s\n' "$tmp" | rg "$fail_pattern" | tail -n 5 >&2 || true
            return 1
          fi
          count="$(printf '%s\n' "$tmp" | rg -c "$pass_pattern" || true)"
        fi
      fi
      if (( count < pass_count )); then
        all_pass=0
      fi
    done
    if (( all_pass )); then
      return 0
    fi
    if (( progress_interval > 0 && SECONDS >= next_progress )); then
      emit_w4_wait_progress "$wait_start" "$pass_count" || true
      next_progress=$((SECONDS + progress_interval))
    fi
    sleep 0.2
  done
  return 2
}

emit_w4_wait_progress() {
  local wait_start="$1"
  local pass_count="$2"
  local elapsed_s=$((SECONDS - wait_start))
  local line
  local summary_parser="$SCRIPT_DIR/w4_guest_run_summary.py"

  if [[ -n "$SIM_UAPI_W5_PROFILE" && -f "$SCRIPT_DIR/w5_inference_cluster_summary.py" ]]; then
    summary_parser="$SCRIPT_DIR/w5_inference_cluster_summary.py"
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    trace "progress: unavailable reason=python3_not_found"
    return 1
  fi

  if ! python3 "$summary_parser" \
    --progress "$RUN_DIR" "$pass_count" "$elapsed_s" "${NODE_IDS[@]}" | while IFS= read -r line; do
      trace "$line"
    done; then
    trace "progress: unavailable reason=progress_parser_failed"
    return 1
  fi
  return 0
}

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! rg -q "$pattern" "$file"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_absent() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if rg -q "$pattern" "$file"; then
    echo "unexpected log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_count() {
  local file="$1"
  local pattern="$2"
  local expected="$3"
  local label="$4"
  local count

  count="$(rg -c "$pattern" "$file" || true)"
  if [[ -z "$count" ]]; then
    count="0"
  fi
  if [[ "$count" != "$expected" ]]; then
    echo "unexpected log marker count: $label expected=$expected actual=$count in $file" >&2
    return 1
  fi
}

assert_no_fatal_runtime_logs() {
  local node_id guest_log qemu_log

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    qemu_log="$RUN_DIR/${node_id}_qemu.log"
    assert_log_absent "$guest_log" "$FATAL_GUEST_PATTERN" "$node_id fatal guest runtime marker" || return 1
    assert_log_absent "$qemu_log" "$FATAL_QEMU_PATTERN" "$node_id fatal qemu runtime marker" || return 1
  done
  return 0
}

node_index() {
  case "$1" in
    nodeA) echo 1 ;;
    nodeB) echo 2 ;;
    nodeC) echo 3 ;;
    nodeD) echo 4 ;;
    nodeE) echo 5 ;;
    nodeF) echo 6 ;;
    nodeG) echo 7 ;;
    nodeH) echo 8 ;;
    *) return 1 ;;
  esac
}

node_ip() {
  local idx="$(node_index "$1")"
  echo "${NODE_IPS[$idx]}"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

trace_run_artifact_paths() {
  local node_id

  if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
    trace "w5_profile: $SIM_UAPI_W5_PROFILE"
  fi
  trace "run_dir: $RUN_DIR"
  trace "control_log: $RUN_DIR/control.log"
  trace "cleanup_script: ${CLEANUP_SCRIPT:-}"
  trace "summary_file: $RUN_SUMMARY_FILE"
  for node_id in "${NODE_IDS[@]}"; do
    trace "${node_id}_guest_log: $RUN_DIR/${node_id}_guest.log"
    trace "${node_id}_qemu_log: $RUN_DIR/${node_id}_qemu.log"
  done
}

write_w4_initramfs_runner() {
  local runner="$RUN_INITRAMFS_DIR/bin/run_app"

  cat > "$runner" <<EOF
#!/bin/busybox sh
set -u

log() {
  echo "[w4guest8:initramfs] \$*"
}

mount_fs() {
  local fstype="\$1"
  local target="\$2"
  /bin/busybox mkdir -p "\$target" >/dev/null 2>&1 || true
  /bin/busybox mount -t "\$fstype" none "\$target" >/dev/null 2>&1 || true
}

bootstrap_fs() {
  mount_fs proc /proc
  mount_fs sysfs /sys
  mount_fs devtmpfs /dev
  mount_fs devpts /dev/pts
}

cmdline_value() {
  local key="\$1"
  local arg
  for arg in \$(/bin/busybox cat /proc/cmdline 2>/dev/null || true); do
    case "\$arg" in
      "\$key"=*)
        echo "\${arg#*=}"
        return 0
        ;;
    esac
  done
  return 1
}

enter_shell() {
  if [ -x /bin/busybox ]; then
    exec /bin/busybox sh
  fi
  if [ -x /bin/sh ]; then
    exec /bin/sh
  fi
  return 1
}

node_role_from_ip() {
  case "\$1" in
    10.0.0.1) echo nodeA ;;
    10.0.0.2) echo nodeB ;;
    10.0.0.3) echo nodeC ;;
    10.0.0.4) echo nodeD ;;
    10.0.0.5) echo nodeE ;;
    10.0.0.6) echo nodeF ;;
    10.0.0.7) echo nodeG ;;
    10.0.0.8) echo nodeH ;;
    *) return 1 ;;
  esac
}

bootstrap_fs

if [ "\${UB_RUN_APP_FROM_INIT:-0}" != "1" ] && [ "\${1-}" != "--resume" ]; then
  log "bootstrap phase: launching /bin/linqu_init"
  UB_RUN_APP_FROM_INIT=1
  export UB_RUN_APP_FROM_INIT
  exec /bin/linqu_init "\$@"
fi

if [ "\${1-}" = "--resume" ]; then
  shift
fi

LINQU_UB_LOCAL_IP="\$(cmdline_value linqu_ipourma_ipv4 || true)"
if [ -z "\$LINQU_UB_LOCAL_IP" ]; then
  log "FAIL: missing linqu_ipourma_ipv4 on kernel cmdline"
  enter_shell
fi

LINQU_UB_ROLE="\$(node_role_from_ip "\$LINQU_UB_LOCAL_IP" || true)"
if [ -z "\$LINQU_UB_ROLE" ]; then
  log "FAIL: unknown linqu_ipourma_ipv4=\$LINQU_UB_LOCAL_IP"
  enter_shell
fi

export LINQU_UB_ROLE
export LINQU_UB_LOCAL_IP
export LINQU_UB_ALL_IPS="$ALL_IPS_CSV"
export LINQU_UB_NODE_COUNT=$SIM_W5_CLUSTER_NODE_COUNT
export LINQU_MEM_SERVICE_CLUSTER=1
export LINQU_W4_REQUIRE_UAPI_RESOURCE=1
export SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION="$SIM_MEM_SERVICE_LAZY_REMOTE_ACTIVATION"
export SIM_UAPI_W5_PROFILE="$SIM_UAPI_W5_PROFILE"
export SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE"
export SIM_W5_RUN_ID="$RUN_ID_BASE"
export SIM_QWEN3_DENSE_MODEL_ID="${SIM_QWEN3_DENSE_MODEL_ID:-}"
export SIM_QWEN3_DENSE_MODEL_KEY="${SIM_QWEN3_DENSE_MODEL_KEY:-}"
export SIM_QWEN3_DENSE_VOCAB_SIZE="${SIM_QWEN3_DENSE_VOCAB_SIZE:-}"
export SIM_QWEN3_DENSE_HIDDEN_SIZE="${SIM_QWEN3_DENSE_HIDDEN_SIZE:-}"
export SIM_QWEN3_DENSE_INTERMEDIATE_SIZE="${SIM_QWEN3_DENSE_INTERMEDIATE_SIZE:-}"
export SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS="${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-}"
export SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS="${SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS:-}"
export SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS="${SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS:-}"
export SIM_QWEN3_DENSE_HEAD_DIM="${SIM_QWEN3_DENSE_HEAD_DIM:-}"
export SIM_QWEN3_DENSE_PREFILL_TOKENS="${SIM_QWEN3_DENSE_PREFILL_TOKENS:-}"
export SIM_QWEN3_DENSE_DECODE_TOKENS="${SIM_QWEN3_DENSE_DECODE_TOKENS:-}"
export SIM_QWEN3_DENSE_TP_NODES="${SIM_QWEN3_DENSE_TP_NODES:-}"
export SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES="${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-}"
export SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES="${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}"
export SIM_QWEN3_DENSE_KV_STATE_BYTES="${SIM_QWEN3_DENSE_KV_STATE_BYTES:-}"
export SIM_QWEN3_GUEST_DECODE_STEP=0
export SIM_QWEN3_GUEST_DECODE_STEPS="$SIM_QWEN3_GUEST_DECODE_STEPS"
export SIM_LLM_INFER_PROMPT_TOKEN_IDS="$SIM_LLM_INFER_PROMPT_TOKEN_IDS"
export SIM_QWEN3_SAMPLER_TOP_K="$SIM_QWEN3_SAMPLER_TOP_K"
export SIM_QWEN3_SAMPLER_TOP_P_MILLI="$SIM_QWEN3_SAMPLER_TOP_P_MILLI"
export SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="$SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI"
export SIM_QWEN3_SAMPLER_SEED="$SIM_QWEN3_SAMPLER_SEED"
export SIM_QWEN3_GUEST_ENGRAM="$SIM_QWEN3_GUEST_ENGRAM"
export SIM_QWEN3_GUEST_ENGRAM_MODE="$SIM_QWEN3_GUEST_ENGRAM_MODE"
export SIM_QWEN3_GUEST_ENGRAM_SESSION_ID="${SIM_QWEN3_GUEST_ENGRAM_SESSION_ID:-guest}"
export SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE="$SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE"
export SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE="$SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE"
export SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI="$SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI"
export SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW="$SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW"
export SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS="$SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS"
export SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION="$SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS"
export SIM_QWEN3_GUEST_ENGRAM_STATE_REF="$SIM_QWEN3_GUEST_ENGRAM_STATE_REF"
export SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF="$SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF"
export SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR"
export SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST"
export SIM_W5_FLASH_WEIGHT_CATALOG="$SIM_W5_FLASH_WEIGHT_CATALOG_GUEST"
export SIM_W5_MEMORY_SERVICE="$SIM_W5_MEMORY_SERVICE"
export SIM_W5_SERVING_REQUEST_ID="$SIM_W5_SERVING_REQUEST_ID"
export SIM_W5_SERVING_REQUESTS_FILE="$SIM_W5_SERVING_REQUESTS_FILE_GUEST"
export SIM_W5_SERVING_REQUEST_COUNT="$SIM_W5_SERVING_REQUEST_COUNT"
export SIM_W5_SERVING_DECODE_STEPS_TOTAL="$SIM_W5_SERVING_DECODE_STEPS_TOTAL"
export SIM_W5_SERVING_QUEUE="$SIM_W5_SERVING_QUEUE"
export SIM_W5_SERVING_INGRESS="$SIM_W5_SERVING_INGRESS"
export SIM_W5_TEST_REQUIRE_PREFIX_CACHE="$SIM_W5_TEST_REQUIRE_PREFIX_CACHE"
export SIM_W5_MEMORY_STORE="$SIM_W5_MEMORY_STORE"
export SIM_W5_MEMORY_OBJECT_STORE="$SIM_W5_MEMORY_OBJECT_STORE"
export SIM_W5_MEMORY_ENGRAM_STATE="$SIM_W5_MEMORY_ENGRAM_STATE"
export SIM_W5_MEMORY_REGISTRY_DIR="$SIM_W5_MEMORY_REGISTRY_DIR"
export SIM_W5_TEST_MEMORY_DECISION_STORE="$SIM_W5_TEST_MEMORY_DECISION_STORE"
export SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE="$SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE"
export SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND="$SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND"
export SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID"
export SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID"
export SIM_W5_TEST_MEMORY_SHORTPATH_ACTION="$SIM_W5_TEST_MEMORY_SHORTPATH_ACTION"
export SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID"
export SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START="$SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START"
export SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END="$SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END"
export SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND"
export SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM"
export SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF"
export SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF="$SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF"
export SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT="$SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT"
export SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START"
export SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END"
export SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION"
export SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM="$SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM"
export SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE="$SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE"
export SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT"
export SIM_W5_TEST_MEMORY_SHORTPATH_STREAM="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM"
export SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH_GUEST"
export SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT"
export SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH_GUEST"
export SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID="$SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID"
export SIM_W5_TEST_MEMORY_PREFETCH_SCOPE="$SIM_W5_TEST_MEMORY_PREFETCH_SCOPE"
export SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX="$SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX"
export SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM"
export SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS"
export SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS"
export SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT"
export SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH_GUEST"
export SIM_W5_TEST_MEMORY_GSVA_KV="$SIM_W5_TEST_MEMORY_GSVA_KV"
export SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH="$SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH"
export SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS"
export SIM_QWEN3_RUNTIME_RANGE_WAIT_MS="$SIM_QWEN3_RUNTIME_RANGE_WAIT_MS"
export SIM_W4_UAPI_COMPLETION_TIMEOUT_MS="$SIM_W4_UAPI_COMPLETION_TIMEOUT_MS"
export SIM_W4_RESOURCE_ASSERTIONS="$SIM_W4_RESOURCE_ASSERTIONS"

DEFAULT_SIM_W5_RUN_ID="\$SIM_W5_RUN_ID"
DEFAULT_SIM_QWEN3_GUEST_DECODE_STEPS="\$SIM_QWEN3_GUEST_DECODE_STEPS"
DEFAULT_SIM_LLM_INFER_PROMPT_TOKEN_IDS="\$SIM_LLM_INFER_PROMPT_TOKEN_IDS"
DEFAULT_SIM_QWEN3_SAMPLER_TOP_K="\$SIM_QWEN3_SAMPLER_TOP_K"
DEFAULT_SIM_QWEN3_SAMPLER_TOP_P_MILLI="\$SIM_QWEN3_SAMPLER_TOP_P_MILLI"
DEFAULT_SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="\$SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI"
DEFAULT_SIM_QWEN3_SAMPLER_SEED="\$SIM_QWEN3_SAMPLER_SEED"
DEFAULT_SIM_W5_TEST_REQUIRE_PREFIX_CACHE="\$SIM_W5_TEST_REQUIRE_PREFIX_CACHE"
export SIM_W5_SERVING_BASE_RUN_ID="\$DEFAULT_SIM_W5_RUN_ID"

is_positive_uint() {
  case "\$1" in
    ""|0|*[!0-9]*)
      return 1
      ;;
  esac
  return 0
}

is_token_csv() {
  case "\$1" in
    ""|,*|*,|*,,*|*[!0-9,]*)
      return 1
      ;;
  esac
  return 0
}

reset_serving_request_env() {
  SIM_W5_RUN_ID="\$DEFAULT_SIM_W5_RUN_ID"
  SIM_W5_SERVING_REQUEST_ID=""
  SIM_QWEN3_GUEST_DECODE_STEPS="\$DEFAULT_SIM_QWEN3_GUEST_DECODE_STEPS"
  SIM_LLM_INFER_PROMPT_TOKEN_IDS="\$DEFAULT_SIM_LLM_INFER_PROMPT_TOKEN_IDS"
  SIM_QWEN3_SAMPLER_TOP_K="\$DEFAULT_SIM_QWEN3_SAMPLER_TOP_K"
  SIM_QWEN3_SAMPLER_TOP_P_MILLI="\$DEFAULT_SIM_QWEN3_SAMPLER_TOP_P_MILLI"
  SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="\$DEFAULT_SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI"
  SIM_QWEN3_SAMPLER_SEED="\$DEFAULT_SIM_QWEN3_SAMPLER_SEED"
  SIM_W5_TEST_REQUIRE_PREFIX_CACHE="\$DEFAULT_SIM_W5_TEST_REQUIRE_PREFIX_CACHE"
}

apply_serving_request_line() {
  local line="\$1"
  local field key value

  reset_serving_request_env
  for field in \$line; do
    case "\$field" in
      \#*)
        break
        ;;
      *=*)
        key="\${field%%=*}"
        value="\${field#*=}"
        ;;
      *)
        log "FAIL: invalid serving request token token=\$field"
        return 1
        ;;
    esac
    case "\$key" in
      request_id)
        SIM_W5_SERVING_REQUEST_ID="\$value"
        ;;
      prompt_token_ids)
        SIM_LLM_INFER_PROMPT_TOKEN_IDS="\$value"
        ;;
      decode_steps)
        SIM_QWEN3_GUEST_DECODE_STEPS="\$value"
        ;;
      sampler_top_k)
        SIM_QWEN3_SAMPLER_TOP_K="\$value"
        ;;
      sampler_top_p_milli)
        SIM_QWEN3_SAMPLER_TOP_P_MILLI="\$value"
        ;;
      sampler_temperature_milli)
        SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="\$value"
        ;;
      sampler_seed)
        SIM_QWEN3_SAMPLER_SEED="\$value"
        ;;
      prefix_cache_required)
        SIM_W5_TEST_REQUIRE_PREFIX_CACHE="\$value"
        ;;
      *)
        log "FAIL: unsupported serving request field key=\$key"
        return 1
        ;;
    esac
  done
  if [ -z "\$SIM_W5_SERVING_REQUEST_ID" ]; then
    log "FAIL: serving request missing request_id"
    return 1
  fi
  if ! is_token_csv "\$SIM_LLM_INFER_PROMPT_TOKEN_IDS"; then
    log "FAIL: serving request has invalid prompt_token_ids request_id=\$SIM_W5_SERVING_REQUEST_ID"
    return 1
  fi
  if ! is_positive_uint "\$SIM_QWEN3_GUEST_DECODE_STEPS"; then
    log "FAIL: serving request has invalid decode_steps request_id=\$SIM_W5_SERVING_REQUEST_ID value=\$SIM_QWEN3_GUEST_DECODE_STEPS"
    return 1
  fi
  case "\$SIM_W5_TEST_REQUIRE_PREFIX_CACHE" in
    0|1)
      ;;
    *)
      log "FAIL: serving request has invalid prefix_cache_required request_id=\$SIM_W5_SERVING_REQUEST_ID value=\$SIM_W5_TEST_REQUIRE_PREFIX_CACHE"
      return 1
      ;;
  esac
  SIM_W5_RUN_ID="\$DEFAULT_SIM_W5_RUN_ID"
  export SIM_W5_RUN_ID
  export SIM_W5_SERVING_REQUEST_ID
  export SIM_QWEN3_GUEST_DECODE_STEPS
  export SIM_LLM_INFER_PROMPT_TOKEN_IDS
  export SIM_QWEN3_SAMPLER_TOP_K
  export SIM_QWEN3_SAMPLER_TOP_P_MILLI
  export SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI
  export SIM_QWEN3_SAMPLER_SEED
  export SIM_W5_TEST_REQUIRE_PREFIX_CACHE
  return 0
}

run_llm_infer_once() {
  local request_index="\${1:-0}"
  local request_step_base="\${2:-\$request_index}"
  local import_pa_bias_mb
  local rc

  import_pa_bias_mb=\$(((request_index + 2) * 4096))
  SIM_W5_SERVING_REQUEST_INDEX="\$request_index"
  SIM_W5_SERVING_DECODE_STEP_BASE="\$request_step_base"
  OBMM_POOL_IMPORT_PA_BIAS_MB="\$import_pa_bias_mb"
  SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB="\$import_pa_bias_mb"
  export SIM_W5_SERVING_REQUEST_INDEX
  export SIM_W5_SERVING_DECODE_STEP_BASE
  export OBMM_POOL_IMPORT_PA_BIAS_MB
  export SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB
  log "start step=0 \$LINQU_UB_ROLE local_ip=\$LINQU_UB_LOCAL_IP request_id=\${SIM_W5_SERVING_REQUEST_ID:-none} run_id=\${SIM_W5_RUN_ID:-none} request_index=\$request_index decode_step_base=\$request_step_base import_pa_bias_mb=\$import_pa_bias_mb"
  set +e
  /bin/linqu_llm_infer
  rc=\$?
  set -e
  if [ "\$rc" = "0" ]; then
    log "linqu_llm_infer completed \$LINQU_UB_ROLE request_id=\${SIM_W5_SERVING_REQUEST_ID:-none}"
    return 0
  fi
  log "FAIL: linqu_llm_infer failed \$LINQU_UB_ROLE request_id=\${SIM_W5_SERVING_REQUEST_ID:-none} rc=\$rc"
  return "\$rc"
}

publish_serving_request_line_to_workers() {
  local line="\$1"
  local request_index="\$2"

  if [ "\$SIM_W5_SERVING_INGRESS" != "nodeA" ] || [ "\$LINQU_UB_ROLE" != "nodeA" ]; then
    return 0
  fi
  log "serving_entry request_publish source=nodeA request_id=\$SIM_W5_SERVING_REQUEST_ID transport=obmm_spsc"
  if ! SIM_MEM_SERVICE_BOOTSTRAP_GENERATION=17 SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB=4096 /bin/linqu_w5_serving_control publish --request-line "\$line" --request-index "\$request_index"; then
    log "FAIL: serving_entry request_publish_failed request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE"
    return 1
  fi
  log "serving_entry request_published source=nodeA request_id=\$SIM_W5_SERVING_REQUEST_ID transport=obmm_spsc"
  return 0
}

run_serving_requests_file() {
  local request_file="\$1"
  local line request_index request_step_base rc

  request_index=0
  request_step_base=0
  if [ ! -f "\$request_file" ]; then
    log "FAIL: serving request file missing path=\$request_file"
    return 1
  fi
  while IFS= read -r line || [ -n "\$line" ]; do
    case "\$line" in
      ""|\#*)
        continue
        ;;
    esac
    if ! apply_serving_request_line "\$line"; then
      return 1
    fi
    if ! publish_serving_request_line_to_workers "\$line" "\$request_index"; then
      return 1
    fi
    log "serving_entry request_start index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID entry=nodeA role=\$LINQU_UB_ROLE decode_steps=\$SIM_QWEN3_GUEST_DECODE_STEPS prompt_token_ids=\$SIM_LLM_INFER_PROMPT_TOKEN_IDS run_id=\$SIM_W5_RUN_ID"
    run_llm_infer_once "\$request_index" "\$request_step_base"
    rc=\$?
    if [ "\$rc" != "0" ]; then
      log "FAIL: serving_entry request_failed index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE rc=\$rc"
      return "\$rc"
    fi
    log "serving_entry request_done index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE"
    request_step_base=\$((request_step_base + SIM_QWEN3_GUEST_DECODE_STEPS))
    request_index=\$((request_index + 1))
  done < "\$request_file"
  if [ "\$request_index" = "0" ]; then
    log "FAIL: serving request file had no runnable requests path=\$request_file"
    return 1
  fi
  log "serving_entry completed requests=\$request_index role=\$LINQU_UB_ROLE"
  return 0
}

run_serving_stdin_queue() {
  local line request_index request_step_base rc

  request_index=0
  request_step_base=0
  log "serving_entry ready mode=serial-line role=\$LINQU_UB_ROLE entry=nodeA"
  while IFS= read -r line; do
    case "\$line" in
      ""|\#*)
        continue
        ;;
      __W5_SERVING_STOP__)
        log "serving_entry stop requests=\$request_index role=\$LINQU_UB_ROLE"
        return 0
        ;;
    esac
    if ! apply_serving_request_line "\$line"; then
      return 1
    fi
    if ! publish_serving_request_line_to_workers "\$line" "\$request_index"; then
      return 1
    fi
    log "serving_entry request_start index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID entry=nodeA role=\$LINQU_UB_ROLE decode_steps=\$SIM_QWEN3_GUEST_DECODE_STEPS prompt_token_ids=\$SIM_LLM_INFER_PROMPT_TOKEN_IDS run_id=\$SIM_W5_RUN_ID"
    run_llm_infer_once "\$request_index" "\$request_step_base"
    rc=\$?
    if [ "\$rc" != "0" ]; then
      log "FAIL: serving_entry request_failed index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE rc=\$rc"
      return "\$rc"
    fi
    log "serving_entry request_done index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE"
    request_step_base=\$((request_step_base + SIM_QWEN3_GUEST_DECODE_STEPS))
    request_index=\$((request_index + 1))
  done < /dev/ttyAMA0
  log "serving_entry queue_closed requests=\$request_index role=\$LINQU_UB_ROLE"
  return 0
}

run_serving_nodea_worker_queue() {
  local request_file
  local line
  local rc
  local request_index
  local request_step_base

  request_index=0
  request_step_base=0
  log "serving_entry ready mode=nodeA-worker role=\$LINQU_UB_ROLE entry=nodeA"
  while :; do
    request_file="/tmp/w5_serving_request.\$LINQU_UB_ROLE.\$request_index"
    rm -f "\$request_file"
    log "serving_entry worker_wait index=\$request_index role=\$LINQU_UB_ROLE source=nodeA transport=obmm_spsc"
    if ! SIM_MEM_SERVICE_BOOTSTRAP_GENERATION=17 SIM_MEM_SERVICE_IMPORT_PA_BIAS_MB=4096 /bin/linqu_w5_serving_control wait --source-node nodeA --request-index "\$request_index" --out "\$request_file"; then
      log "FAIL: serving_entry worker_wait_failed index=\$request_index role=\$LINQU_UB_ROLE source=nodeA"
      return 1
    fi
    if ! IFS= read -r line < "\$request_file"; then
      log "FAIL: serving_entry worker_request_missing index=\$request_index role=\$LINQU_UB_ROLE source=nodeA path=\$request_file"
      return 1
    fi
    if ! apply_serving_request_line "\$line"; then
      return 1
    fi
    log "serving_entry worker_received index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE source=nodeA transport=obmm_spsc decode_steps=\$SIM_QWEN3_GUEST_DECODE_STEPS prompt_token_ids=\$SIM_LLM_INFER_PROMPT_TOKEN_IDS"
    run_llm_infer_once "\$request_index" "\$request_step_base"
    rc=\$?
    if [ "\$rc" != "0" ]; then
      log "FAIL: serving_entry request_failed index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE rc=\$rc"
      return "\$rc"
    fi
    log "serving_entry request_done index=\$request_index request_id=\$SIM_W5_SERVING_REQUEST_ID role=\$LINQU_UB_ROLE"
    request_step_base=\$((request_step_base + SIM_QWEN3_GUEST_DECODE_STEPS))
    request_index=\$((request_index + 1))
  done
}

if [ "\$SIM_W5_SERVING_QUEUE" = "1" ]; then
  if [ "\$SIM_W5_SERVING_INGRESS" = "nodeA" ] && [ "\$LINQU_UB_ROLE" != "nodeA" ]; then
    run_serving_nodea_worker_queue
  else
    run_serving_stdin_queue
  fi
elif [ -n "\$SIM_W5_SERVING_REQUESTS_FILE" ]; then
  run_serving_requests_file "\$SIM_W5_SERVING_REQUESTS_FILE"
else
  run_llm_infer_once
fi

log "entering shell after w4 guest runner"
enter_shell
EOF
  chmod +x "$runner"
}

build_w4_initramfs() {
  local base_initramfs="$OUT_DIR/initramfs.cpio.gz"

  trace "prepare: build per-run initramfs image=$RUN_INITRAMFS_IMAGE"
  ensure_ub_guest_artifacts "$ROOT_DIR" "$OUT_DIR/Image" "$base_initramfs" || return 1
  rm -rf "$RUN_INITRAMFS_DIR" "$RUN_INITRAMFS_IMAGE"
  mkdir -p "$RUN_INITRAMFS_DIR"
  (
    cd "$RUN_INITRAMFS_DIR"
    gzip -dc "$base_initramfs" | cpio -id --quiet
  )
  stage_qwen3_object_service_snapshot || return 1
  stage_flash_weight_catalog || return 1
  stage_w5_memory_shortpath_stream || return 1
  stage_w5_memory_shortpath_kv_stream || return 1
  stage_w5_memory_prefix_cache_kv_stream || return 1
  stage_w5_serving_requests_file || return 1
  write_w4_initramfs_runner || return 1
  (
    cd "$RUN_INITRAMFS_DIR"
    find . -print | cpio -o -H newc --quiet | gzip -9 > "$RUN_INITRAMFS_IMAGE"
  )
}

write_w5_openEuler_systemd_unit() {
  # $1 = target dir (…/etc/systemd/system); replace the canonical guest unit
  # so an image carrying the legacy launcher cannot start run_app a second time.
  local unit_dir="$1"
  cat > "$unit_dir/linqu-w5-guest.service" <<'UNIT'
[Unit]
Description=UB SIM W5 guest runner (openEuler engine)
After=network.target

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=0
ExecStartPre=-/usr/bin/systemctl stop firewalld
StandardOutput=tty
StandardError=tty
TTYPath=/dev/ttyAMA0
ExecStart=/bin/busybox sh /bin/run_app

[Install]
WantedBy=multi-user.target
UNIT
}

build_w4_openEuler_initramfs() {
  local base_initramfs="$OUT_DIR/initramfs.cpio.gz"
  local overlay_dir

  trace "prepare: build per-run openEuler initramfs image=$RUN_INITRAMFS_IMAGE"
  ensure_ub_guest_artifacts "$ROOT_DIR" "$OUT_DIR/Image" "$base_initramfs"
  oe_ensure_lvm2_staging "$SIM_W5_OE_DISK_IMAGE" || return 1
  rm -rf "$RUN_INITRAMFS_DIR" "$RUN_INITRAMFS_IMAGE"
  mkdir -p "$RUN_INITRAMFS_DIR"
  (
    cd "$RUN_INITRAMFS_DIR"
    gzip -dc "$base_initramfs" | cpio -id --quiet
  )
  stage_qwen3_object_service_snapshot || return 1
  stage_flash_weight_catalog || return 1
  stage_w5_memory_shortpath_stream || return 1
  stage_w5_memory_shortpath_kv_stream || return 1
  stage_w5_memory_prefix_cache_kv_stream || return 1
  stage_w5_serving_requests_file || return 1
  write_w4_initramfs_runner || return 1
  # openEuler boot half: /init becomes init_switch_root; add LVM2 userland and
  # the UB modules that ship as =m on top of the unpacked base tree.
  oe_build_boot_skeleton "$ROOT_DIR" "$RUN_INITRAMFS_DIR/bin/busybox" "$RUN_INITRAMFS_DIR" || return 1
  # Root overlay deployed verbatim by init_switch_root: the same run_app flow,
  # the binaries it execs at busybox-flow paths, staged /tmp payloads, and a
  # systemd unit so openEuler starts run_app after multi-user.target.
  overlay_dir="$RUN_INITRAMFS_DIR/ub_root_overlay"
  mkdir -p "$overlay_dir/bin" "$overlay_dir/tmp" \
    "$overlay_dir/etc/systemd/system/multi-user.target.wants"
  cp -a "$RUN_INITRAMFS_DIR/bin/busybox" "$overlay_dir/bin/busybox"
  for app_bin in "$RUN_INITRAMFS_DIR"/bin/linqu_*; do
    [[ -f "$app_bin" ]] || continue
    cp -a "$app_bin" "$overlay_dir/bin/"
  done
  cp -a "$RUN_INITRAMFS_DIR/bin/run_app" "$overlay_dir/bin/run_app"
  cp -a "$RUN_INITRAMFS_DIR/tmp/." "$overlay_dir/tmp/" 2>/dev/null || true
  write_w5_openEuler_systemd_unit "$overlay_dir/etc/systemd/system"
  ln -s ../linqu-w5-guest.service \
    "$overlay_dir/etc/systemd/system/multi-user.target.wants/linqu-w5-guest.service"
  trace "prepare: openEuler root overlay staged dir=$overlay_dir disk=$SIM_W5_OE_DISK_IMAGE"
  (
    cd "$RUN_INITRAMFS_DIR"
    find . -print | cpio -o -H newc --quiet | gzip -9 > "$RUN_INITRAMFS_IMAGE"
  )
}

append_run_artifact_cleanup() {
  if [[ -n "${CLEANUP_SCRIPT:-}" && -f "$CLEANUP_SCRIPT" ]]; then
    cat >> "$CLEANUP_SCRIPT" <<EOF
rm -rf '$RUN_INITRAMFS_DIR'
rm -f '$RUN_INITRAMFS_IMAGE'
EOF
  fi
}

validate_owner_observed() {
  local node_id="$1"
  local log_file="$2"
  local owner_idx="$3"
  local owner_role="$4"

  assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage db_service_cluster_observe owner=node${owner_idx} kind=prefix_group key=request/w4-${owner_role}-request-0/prefix-group/${owner_role}-group-0 group=${owner_role}-group-0 members=2 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} group" || return 1
  assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage db_service_cluster_observe owner=node${owner_idx} kind=request_prefix key=request/w4-${owner_role}-request-0/prefix/${owner_role}-prefix-0 version=[0-9]+" "$node_id saw ${owner_role} prefix" || return 1
  assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-0 state=hot version=[0-9]+" "$node_id saw ${owner_role} block0" || return 1
  assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-1 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} block1" || return 1
}

qwen3_engram_context_refs_configured() {
  [[ -n "$SIM_QWEN3_GUEST_ENGRAM_STATE_REF" ]]
}

qwen3_engram_context_op_enabled() {
  [[ -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" &&
    "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" != "disabled" &&
    "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" != "none" ]]
}

w5_shortpath_execute_enabled() {
  case "${SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE:-0}" in
    1|true|TRUE|yes|YES)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

w5_shortpath_execution_armed() {
  if ! w5_shortpath_execute_enabled; then
    return 1
  fi
  if [[ "${SIM_W5_TEST_MEMORY_SHORTPATH_ACTION:-}" != "jump-to-terminal" ]]; then
    return 1
  fi
  if [[ -n "${SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF:-}" ||
        -n "${SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF:-}" ||
        -n "${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM:-}" ||
        -n "${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH:-}" ]]; then
    return 0
  fi
  return 1
}

runtime_boundary_lookup_produces_store_after_guest() {
  case "${SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-0}" in
    1|true|TRUE|yes|YES)
      if ! w5_shortpath_execution_armed; then
        return 0
      fi
      ;;
  esac
  return 1
}

validate_qwen3_engram_context_refs() {
  if [[ "$SIM_QWEN3_GUEST_ENGRAM" != "1" ]] || ! qwen3_engram_context_op_enabled; then
    return 0
  fi
  if [[ -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF" ||
        -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF" ||
        -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF" ]]; then
    trace "FAIL: qwen3 engram context component refs are not a real W5 entrypoint hint=materialize Lingqu Memory Service context objects and export SIM_QWEN3_GUEST_ENGRAM_STATE_REF only"
    return 1
  fi
  if ! qwen3_engram_context_refs_configured; then
    trace "FAIL: qwen3 engram context op requires EngramStateObjectRef context_op=$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP hint=materialize Lingqu Memory Service context objects and export SIM_QWEN3_GUEST_ENGRAM_STATE_REF"
    return 1
  fi
  return 0
}

validate_w5_engram_context_summary() {
  if ! qwen3_engram_context_refs_configured; then
    return 0
  fi
  if [[ ! -f "$RUN_SUMMARY_FILE" ]]; then
    trace "FAIL: qwen3 engram context object-ref summary missing path=$RUN_SUMMARY_FILE"
    return 1
  fi
  if ! rg -q "engram_context_summary: records=[1-9][0-9]* steps=${SIM_QWEN3_GUEST_DECODE_STEPS}/${SIM_QWEN3_GUEST_DECODE_STEPS} modes=[^ ]*object-ref" "$RUN_SUMMARY_FILE"; then
    trace "FAIL: qwen3 engram context summary missing object-ref mode path=$RUN_SUMMARY_FILE"
    return 1
  fi
  return 0
}

validate_w5_boundary_observation_summary() {
  local idle_expected
  local node_id
  local stale_summary_pattern

  if [[ -z "$SIM_UAPI_W5_PROFILE" ]]; then
    return 0
  fi
  if is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    return 0
  fi
  if [[ ! -f "$RUN_SUMMARY_FILE" ]]; then
    trace "FAIL: W5 boundary observation summary missing path=$RUN_SUMMARY_FILE"
    return 1
  fi
  if w5_shortpath_execution_armed; then
    idle_expected=$((SIM_QWEN3_GUEST_DECODE_STEPS * (${#NODE_IDS[@]} - 1)))
    stale_summary_pattern="timing_node: node=node[BCDEFGH] steps=0/${SIM_QWEN3_GUEST_DECODE_STEPS} status=missing|handoff_node: node=node[BCDEFGH] steps=0/${SIM_QWEN3_GUEST_DECODE_STEPS} status=missing|model_range_kv_state_lazy_fallback|fallback=runtime_forward_metadata|w5_shortpath_decision:|payload_bytes=[0-9]+,[0-9]+|checksums=0x|obmm_pool: unavailable"
    if rg -q "memory_service_summary: .*qwen3_w5_memory_shortpath_commit:${SIM_QWEN3_GUEST_DECODE_STEPS}(,| )" "$RUN_SUMMARY_FILE" &&
      rg -q "memory_service_summary: .*qwen3_w5_memory_terminal_logits_selected:${SIM_QWEN3_GUEST_DECODE_STEPS}(,| )" "$RUN_SUMMARY_FILE" &&
      rg -q "memory_service_summary: .*lookup_hits=${SIM_QWEN3_GUEST_DECODE_STEPS}( |$)" "$RUN_SUMMARY_FILE" &&
      rg -q "memory_service_summary: .*actions=jump-to-terminal .*artifact_kinds=logits" "$RUN_SUMMARY_FILE" &&
      ! rg -q "memory_service_summary: .*shortpath_ids=none" "$RUN_SUMMARY_FILE" &&
      ! rg -q "memory_service_summary: .*support_ids=none" "$RUN_SUMMARY_FILE" &&
      ! rg -q "memory_service_summary: .*artifact_kinds=none" "$RUN_SUMMARY_FILE"; then
      if ! rg -q "summary: .*worker_timing_records=${SIM_QWEN3_GUEST_DECODE_STEPS} .*idle_timing_records=${idle_expected}" "$RUN_SUMMARY_FILE"; then
        trace "FAIL: W5 shortpath timing record counts are incomplete expected_active=${SIM_QWEN3_GUEST_DECODE_STEPS} expected_idle=${idle_expected} path=$RUN_SUMMARY_FILE"
        return 1
      fi
      for node_id in "${NODE_IDS[@]:1}"; do
        if ! rg -q "timing_node: node=${node_id} steps=0/${SIM_QWEN3_GUEST_DECODE_STEPS} idle_steps=${SIM_QWEN3_GUEST_DECODE_STEPS}/${SIM_QWEN3_GUEST_DECODE_STEPS} .*status=idle_no_work_item" "$RUN_SUMMARY_FILE"; then
          trace "FAIL: W5 shortpath downstream timing is not idle-only node=$node_id path=$RUN_SUMMARY_FILE"
          return 1
        fi
        if ! rg -q "handoff_node: node=${node_id} steps=0/${SIM_QWEN3_GUEST_DECODE_STEPS} idle_steps=${SIM_QWEN3_GUEST_DECODE_STEPS}/${SIM_QWEN3_GUEST_DECODE_STEPS} .*status=idle_no_work_item" "$RUN_SUMMARY_FILE"; then
          trace "FAIL: W5 shortpath downstream handoff is not idle-only node=$node_id path=$RUN_SUMMARY_FILE"
          return 1
        fi
      done
      if ! rg -q "obmm_pool: not_observed reason=no_obmm_pool_usage_records active_worker_records=${SIM_QWEN3_GUEST_DECODE_STEPS} idle_worker_records=${idle_expected}" "$RUN_SUMMARY_FILE"; then
        trace "FAIL: W5 shortpath pool usage summary is ambiguous path=$RUN_SUMMARY_FILE"
        return 1
      fi
      if ! rg -q "guest_worker_shortpath_summary: action=jump-to-terminal boundary_hits=${SIM_QWEN3_GUEST_DECODE_STEPS} terminal_selects=${SIM_QWEN3_GUEST_DECODE_STEPS} expected_hits=${SIM_QWEN3_GUEST_DECODE_STEPS} actual_range_forwards=${SIM_QWEN3_GUEST_DECODE_STEPS} actual_runtime_inputs=$((SIM_QWEN3_GUEST_DECODE_STEPS - 1)) actual_runtime_outputs=0 shortpath_no_dispatch=${idle_expected} shortpath_terminal_commits=${idle_expected} shortpath_publish_hidden_zero=${SIM_QWEN3_GUEST_DECODE_STEPS} full_pipeline_range_forwards=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]})) full_pipeline_runtime_inputs=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]} - 1)) full_pipeline_runtime_outputs=$((SIM_QWEN3_GUEST_DECODE_STEPS * ${#NODE_IDS[@]}))" "$RUN_SUMMARY_FILE"; then
        trace "FAIL: W5 shortpath worker summary does not prove reduced range pipeline path=$RUN_SUMMARY_FILE"
        return 1
      fi
      if rg -q "$stale_summary_pattern" "$RUN_SUMMARY_FILE"; then
        trace "FAIL: W5 shortpath summary contains stale fallback/missing/ambiguous markers path=$RUN_SUMMARY_FILE"
        return 1
      fi
      return 0
    fi
    trace "FAIL: W5 shortpath execution summary incomplete or unauditable expected_steps=$SIM_QWEN3_GUEST_DECODE_STEPS path=$RUN_SUMMARY_FILE"
    return 1
  fi
  if ! rg -q "memory_boundary_observation_summary: records=[1-9][0-9]* steps=${SIM_QWEN3_GUEST_DECODE_STEPS}/${SIM_QWEN3_GUEST_DECODE_STEPS} .*source=w5_guest_range_exit hidden_backend=obmm_shmem" "$RUN_SUMMARY_FILE"; then
    trace "FAIL: W5 boundary observation summary incomplete path=$RUN_SUMMARY_FILE"
    return 1
  fi
  if ! rg -q "memory_boundary_observation: phase=range_exit observation_id=boundary-observation/${RUN_ID_BASE}/step[0-9]+/node[1-7] .*backing=obmm_shmem metadata=lingqu_object_service .*status=ok" "$RUN_SUMMARY_FILE"; then
    trace "FAIL: W5 boundary observation ids missing guest run id run_id=$RUN_ID_BASE path=$RUN_SUMMARY_FILE"
    return 1
  fi
  return 0
}

file_size_bytes() {
  local path="$1"
  local -A file_stat

  zmodload zsh/stat
  zstat -H file_stat +size -- "$path"
  printf '%s\n' "$file_stat[size]"
}

validate_w5_artifact_file_size() {
  local path="$1"
  local label="$2"
  local max_bytes="$3"
  local required="${4:-1}"
  local bytes

  if [[ -z "$path" ]]; then
    return 0
  fi
  if [[ ! -f "$path" ]]; then
    if [[ "$required" != "1" ]]; then
      trace "W5 artifact size skipped label=$label reason=not_materialized_yet path=$path"
      return 0
    fi
    trace "FAIL: W5 artifact size check missing label=$label path=$path"
    return 1
  fi
  bytes="$(file_size_bytes "$path")"
  if (( bytes > max_bytes )); then
    trace "FAIL: W5 artifact size too large label=$label bytes=$bytes max_bytes=$max_bytes path=$path"
    return 1
  fi
  trace "W5 artifact size ok label=$label bytes=$bytes max_bytes=$max_bytes path=$path"
  return 0
}

compute_w5_object_store_bin_max_bytes() {
  local base_bytes="${1:-268435456}"
  local steps="${SIM_QWEN3_GUEST_DECODE_STEPS:-0}"
  local per_step_bytes=$((24 * 1024 * 1024))
  local step_budget

  if ! [[ "$steps" == <-> ]]; then
    steps=0
  fi
  step_budget=$((steps * per_step_bytes))
  if (( step_budget > base_bytes )); then
    printf '%s\n' "$step_budget"
    return 0
  fi
  printf '%s\n' "$base_bytes"
}

validate_w5_artifact_sizes() {
  local object_json="${SIM_W5_MEMORY_OBJECT_STORE:-}"
  local object_bin=""
  local memory_json="${SIM_W5_MEMORY_STORE:-}"
  local memory_bin=""
  local registry_dir="${SIM_W5_MEMORY_REGISTRY_DIR:-${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}}"
  local shortpath_stream="${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH:-}"
  local shortpath_kv_stream="${SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH:-}"
  local prefix_cache_kv_stream="${SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH:-}"
  local max_memory_json="${SIM_W5_TEST_MAX_MEMORY_STORE_JSON_BYTES:-16777216}"
  local max_object_json="${SIM_W5_TEST_MAX_OBJECT_STORE_JSON_BYTES:-8388608}"
  local max_object_bin="${SIM_W5_TEST_MAX_OBJECT_STORE_BIN_BYTES:-268435456}"
  local max_shortpath_stream="${SIM_W5_TEST_MAX_SHORTPATH_STREAM_BYTES:-1048576}"
  local max_shortpath_kv_stream="${SIM_W5_TEST_MAX_SHORTPATH_KV_STREAM_BYTES:-1048576}"
  local max_prefix_cache_kv_stream="${SIM_W5_TEST_MAX_PREFIX_CACHE_KV_STREAM_BYTES:-1048576}"
  local store_required=1
  local shortpath_required=0
  local prefix_cache_required=0

  max_object_bin="$(compute_w5_object_store_bin_max_bytes "$max_object_bin")"

  if runtime_boundary_lookup_produces_store_after_guest; then
    store_required=0
  fi
  if [[ -z "$SIM_UAPI_W5_PROFILE" ]]; then
    return 0
  fi
  if is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    validate_w5_artifact_file_size \
      "${SIM_W5_FLASH_WEIGHT_CATALOG:-}" \
      "flash_weight_catalog" \
      67108864 \
      1 || return 1
    return 0
  fi
  if [[ -n "${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}" ]]; then
    shortpath_required=0
  fi
  if [[ "$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT" == *.json && ( -z "$object_json" || ! -f "$object_json" ) ]]; then
    object_json="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT"
  fi
  if [[ -n "$object_json" ]]; then
    if [[ "$object_json" == *.json ]]; then
      object_bin="${object_json%.json}.bin"
    else
      object_bin="${object_json}.bin"
    fi
  fi
  if [[ -n "$memory_json" ]]; then
    if [[ "$memory_json" == *.json ]]; then
      memory_bin="${memory_json%.json}.bin"
    else
      memory_bin="${memory_json}.bin"
    fi
  fi
  if [[ -n "$registry_dir" && ( -z "$shortpath_stream" || "$shortpath_stream" == /tmp/* || ! -f "$shortpath_stream" ) ]]; then
    shortpath_stream="$registry_dir/w5_memory_shortpath_stream.txt"
  fi
  if [[ -n "$registry_dir" && ( -z "$shortpath_kv_stream" || "$shortpath_kv_stream" == /tmp/* || ! -f "$shortpath_kv_stream" ) ]]; then
    shortpath_kv_stream="$registry_dir/w5_memory_shortpath_kv_stream.txt"
  fi
  if [[ -n "$registry_dir" && ( -z "$prefix_cache_kv_stream" || "$prefix_cache_kv_stream" == /tmp/* || ! -f "$prefix_cache_kv_stream" ) ]]; then
    prefix_cache_kv_stream="$registry_dir/w5_memory_prefix_cache_kv_stream.txt"
  fi
  if [[ -z "${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}" && ( -n "${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH:-}" || -n "${SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH:-}" ) ]]; then
    shortpath_required="$store_required"
  fi
  if [[ -n "${SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH:-}" ]]; then
    prefix_cache_required="$store_required"
  fi

  validate_w5_artifact_file_size "$memory_json" "memory_store_json" "$max_memory_json" "$store_required" || return 1
  validate_w5_artifact_file_size "$memory_bin" "memory_store_bin" "$max_object_bin" "0" || return 1
  validate_w5_artifact_file_size "$object_json" "object_store_json" "$max_object_json" "$store_required" || return 1
  validate_w5_artifact_file_size "$object_bin" "object_store_bin" "$max_object_bin" "$store_required" || return 1
  validate_w5_artifact_file_size "$shortpath_stream" "shortpath_stream" "$max_shortpath_stream" "$shortpath_required" || return 1
  validate_w5_artifact_file_size "$shortpath_kv_stream" "shortpath_kv_stream" "$max_shortpath_kv_stream" "$shortpath_required" || return 1
  validate_w5_artifact_file_size "$prefix_cache_kv_stream" "prefix_cache_kv_stream" "$max_prefix_cache_kv_stream" "$prefix_cache_required" || return 1
  return 0
}

emit_w5_inference_run_report() {
  local line
  local -a report_flags=()
  local report_parser="$SCRIPT_DIR/w5_inference_run_report.py"

  if [[ -z "$SIM_UAPI_W5_PROFILE" ]]; then
    return 0
  fi
  if [[ ! -x "$report_parser" ]]; then
    trace "FAIL: W5 inference run report parser missing path=$report_parser"
    return 1
  fi
  if [[ ! -f "$RUN_SUMMARY_FILE" ]]; then
    trace "FAIL: W5 inference run report summary missing path=$RUN_SUMMARY_FILE"
    return 1
  fi
  case "${SIM_W5_TEST_REQUIRE_PREFIX_CACHE}" in
    1|true|TRUE|yes|YES)
      report_flags+=(--require-prefix-cache)
      ;;
  esac
  if ! python3 "$report_parser" "${report_flags[@]}" "$RUN_SUMMARY_FILE" | while IFS= read -r line; do
    trace "$line"
  done; then
    trace "FAIL: W5 inference run report validation failed path=$RUN_SUMMARY_FILE"
    return 1
  fi
  return 0
}

run_w5_artifact_size_validation_cli() {
  if (( $# != 0 )); then
    echo "--validate-w5-artifact-sizes-only does not accept positional arguments" >&2
    return 2
  fi

  mkdir -p "${TRACE_FILE:h}"
  : > "$TRACE_FILE"
  validate_w5_artifact_sizes
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local qemu_log="$RUN_DIR/${node_id}_qemu.log"
  local expected_dispatch_word="0x41a0000041a00000"
  local engram_candidates_owner_node="$SIM_W5_CLUSTER_NODE_COUNT"
  local terminal_publish_node="$SIM_W5_CLUSTER_NODE_COUNT"
  local idx owner_role
  local remote_idx

  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "host_matmul" ]]; then
    expected_dispatch_word="0x3f8000003f800000"
  elif is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    expected_dispatch_word="0x[0-9a-f]+"
  elif is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    expected_dispatch_word="0x0000000000000000"
  fi
  idx="$(node_index "$node_id")"
  remote_idx=$((idx % SIM_W5_CLUSTER_NODE_COUNT + 1))

  if [[ -n "$SIM_UAPI_W5_PROFILE" ]] && w5_shortpath_execution_armed; then
    if (( idx > 1 )); then
      assert_log_count "$log_file" "\\[w4_guest\\] stage model_decode_round_scheduler_no_dispatch .*reason=terminal_token_committed.*work_item=none.*status=no_dispatch" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath scheduler no-dispatch per step" || return 1
      assert_log_count "$log_file" "\\[(w4_guest|mem_service)\\] stage model_decode_round_terminal_committed .*target=decode_round_scheduler.*status=committed" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath terminal commit observed per step" || return 1
      assert_log_count "$log_file" "\\[w4_guest\\] stage model_decode_round_idle_timing .*terminal_observed=1.*status=idle" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath idle timing per step" || return 1
      assert_log_absent "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_forward_runtime_output_publish local=node${idx} " "$node_id W5 shortpath downstream runtime output publish" || return 1
      assert_log_absent "$log_file" "\\[w4_guest\\] stage model_worker_handoff_timing local=${node_id} " "$node_id W5 shortpath downstream handoff timing" || return 1
      assert_log_absent "$log_file" "\\[w4_guest\\] stage uapi_model_range_runtime_forward node=$((idx - 1)) " "$node_id W5 shortpath downstream range forward" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
      assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
      return 0
    fi
    assert_log_count "$log_file" "\\[w4_guest\\] stage qwen3_w5_memory_shortpath_commit .*publish_hidden=0.*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath boundary commit per step" || return 1
    assert_log_count "$log_file" "\\[w4_guest\\] stage qwen3_w5_memory_terminal_logits_selected .*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath terminal logits selected per step" || return 1
    assert_log_count "$log_file" "\\[(w4_guest|mem_service)\\] stage model_terminal_token_result_publish .*publisher=shortpath_boundary" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id W5 shortpath terminal token publish per step" || return 1
    assert_log_absent "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_forward_runtime_output_publish local=node${idx} " "$node_id W5 shortpath boundary runtime output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
    assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
    return 0
  fi

  if [[ -n "$SIM_UAPI_W5_PROFILE" ]] &&
    rg -q "\\[w4_guest\\] stage model_decode_round_scheduler_no_dispatch .* work_item=none .* status=no_dispatch" "$log_file"; then
    assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
    assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
    return 0
  fi
  if [[ -n "$SIM_UAPI_W5_PROFILE" ]] &&
    rg -q "\\[w4_guest\\] stage qwen3_w5_memory_shortpath_commit .* publish_hidden=0 status=ok" "$log_file"; then
    assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
    assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
    return 0
  fi

  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_kvcache_path=ready" "$node_id obmm kvcache backing" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_cluster_mode=resource_backed_uapi" "$node_id db cluster resource-backed mode" || return 1
  if is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_cluster_runtime_bootstrap local=node${idx} nodes=$SIM_W5_CLUSTER_NODE_COUNT backing=obmm_pool metadata=lingqu_object_service queue=obmm_spsc status=ok" "$node_id explicit obmm cluster runtime bootstrap" || return 1
  fi
  if [[ "$SIM_W4_RESOURCE_ASSERTIONS" == "1" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster=resource_backed_assertions_(ok|skipped) nodes=8 .*" "$node_id resource-backed db cluster assertions" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_publish kind=weight_tile key=weights/qwen3[-.0-9a-z]*/node${idx}/tile0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm weight publish" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_publish kind=kvcache_block key=kvcache/w4/node${idx}/block0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm kvcache publish" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_publish kind=hidden_range_input key=hidden/qwen3[-.0-9a-z]*/node${idx}/range-input owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range input publish" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_publish kind=hidden_range_output key=hidden/qwen3[-.0-9a-z]*/node${idx}/range-output owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range output publish" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_range_forward_placement local=node${idx} key=placement/qwen3[-.0-9a-z]*/layer-range/node${idx} .* next=node${remote_idx} .* source=db_metadata strategy=balanced_layers status=ok" "$node_id qwen3 range forward placement" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_range_forward_contract local=node${idx} .* pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=[1-9][0-9]* .* balanced=true .*placement_source=db_metadata .*backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward contract" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_object_desc_put local=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor put" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_object_desc_get remote=node${remote_idx} reader=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor get" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_resolve kind=weight_tile key=weights/qwen3[-.0-9a-z]*/node${remote_idx}/tile0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote weight resolve" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_resolve kind=kvcache_block key=kvcache/w4/node${remote_idx}/block0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote kvcache resolve" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_resolve kind=hidden_range_input key=hidden/qwen3[-.0-9a-z]*/node${remote_idx}/range-input owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range input resolve" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0_resolve kind=hidden_range_output key=hidden/qwen3[-.0-9a-z]*/node${remote_idx}/range-output owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range output resolve" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_range_forward_handoff local=node${idx} next=node${remote_idx} .* placement_source=db_metadata backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 range forward handoff" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_range_forward_summary local=node${idx} nodes=8 layers=[1-9][0-9]* .* hidden_bytes=[1-9][0-9]* objects=2 .* balanced=true placement_source=db_metadata backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward summary" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage obmm_service_v0=payload_backing_resolved local=node${idx} remote=node${remote_idx} objects=4 bytes=8192 hidden_bytes=[1-9][0-9]* boundary_offsets=0,248,256,4088,4096 backing=obmm_pool metadata=db status=ok" "$node_id obmm payload backing resolved" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=open_resource ok path=" "$node_id uapi resource opened" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_endpoint ok" "$node_id uapi endpoint mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_queues ok" "$node_id uapi queues mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=queue_phys ok" "$node_id uapi queue phys" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=read_default_segment ok segment=[0-9]+" "$node_id uapi default segment" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_seeded segment=[0-9]+ bytes=8192 checksum=0x[0-9a-f]+" "$node_id uapi kvcache payload seeded" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_boundaries segment=[0-9]+ offsets=0,248,256,4088,4096,4104 status=ok" "$node_id uapi kvcache payload boundaries" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=128 puts=1 gets=1 role=hot_shared" "$node_id uapi kvcache shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=8192 puts=1 gets=1 role=legacy_kvcache_payload" "$node_id uapi kvcache boundary shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]*" "$node_id uapi kvcache db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* role=aux_block" "$node_id uapi kvcache aux db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ writes=1 reads=1" "$node_id uapi kvcache block descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-1 segment=[0-9]+ writes=1 reads=1 role=aux_block_boundary" "$node_id uapi kvcache aux block descriptor" || return 1
  if is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_dispatch_descriptor node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) segment=[0-9]+ task_id=31 object_ref_table_offset=0x[0-9a-f]+ object_ref_count=[0-9]+ source=db_metadata status=ok" "$node_id model range dispatch descriptor" || return 1
  else
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_chipbackend_dispatch_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ task_id=31" "$node_id uapi chipbackend descriptor" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=doorbell ok slots=15" "$node_id uapi doorbell" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=wait_completions ok cq_tail=15" "$node_id uapi completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=decode_completions ok" "$node_id decode completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_dispatch_result segment=[0-9]+ word0=${expected_dispatch_word}" "$node_id dispatch payload result" || return 1
  if is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_compute_contract node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=[1-9][0-9]* hidden_bytes=[1-9][0-9]* source=(dispatch_task|runtime_forward) output=(completion|metadata) status=ok" "$node_id model range compute contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_runtime_forward node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=[1-9][0-9]* hidden_bytes=[1-9][0-9]* input_checksum=0x[0-9a-f]+ output_checksum=0x[0-9a-f]+ range_checksum=0x[0-9a-f]+ real_layers=[0-9]+ payload_offset=0x[0-9a-f]+ payload_bytes=[1-9][0-9]* kv_payload_offset=0x[0-9a-f]+ kv_payload_bytes=[1-9][0-9]* kv_payload_checksum=0x[0-9a-f]+ source=runtime_forward output=metadata status=ok" "$node_id qwen3 range runtime forward" || return 1
    if (( idx > 1 )); then
      assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_forward_runtime_input_loaded node=${idx} layers=\\[[0-9]+,[0-9]+\\) input_offset=0x[0-9a-f]+ input_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* source=obmm_object_view target=uapi_object_ref materialize=none status=ok inline_payload=0" "$node_id model runtime range input loaded" || return 1
    fi
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]] && qwen3_engram_context_refs_configured; then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_state_object_ref local=${node_id} node=${idx} state_ref_chars=[1-9][0-9]* manifest_bytes=[1-9][0-9]* registry_dir=.* source=env_contract target=engram_state_manifest status=ok" "$node_id qwen3 engram state ref configured" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_context_object_refs_loaded node=${idx} step=[0-9]+ refs=1 state_bytes=[1-9][0-9]* state_checksum=0x[0-9a-f]+ source=engram_state_ref target=uapi_object_ref status=ok" "$node_id qwen3 engram state ref loaded" || return 1
    fi
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_forward_runtime_output_publish local=node${idx} step=[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ output_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ producer_publish_mono_ms=[0-9]+ producer_clock_offset_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok" "$node_id model runtime range output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage model_worker_handoff_timing local=${node_id} step=[0-9]+ node=${idx} .* producer_to_input_found_mono_ms=-?[0-9]+ .* input_found_to_handoff_ms=[0-9]+ .* status=ok" "$node_id model worker handoff timing" || return 1
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_timing local=${node_id} step=[0-9]+ node=${idx} owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} candidate_publish_ms=[0-9]+ candidate_wait_ms=[0-9]+ policy_select_ms=[0-9]+ decision_publish_ms=[0-9]+ selected_wait_ms=[0-9]+ selected_writeback_ms=[0-9]+ history_state_wait_ms=[0-9]+ qwen3_range_publish_ms=[0-9]+ qwen3_range_input_wait_ms=[0-9]+ status=ok" "$node_id qwen3 engram timing" || return 1
    fi
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_kv_state_publish local=node${idx} step=[0-9]+ key=kvcache/qwen3[-.0-9a-z]*(/scope/[0-9a-f]{16})?/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ slot_bytes=[1-9][0-9]* block_bytes=[1-9][0-9]* blocks=[1-9][0-9]* reserved_bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_shmem metadata=lingqu_object_service status=ok" "$node_id model range kv state publish" || return 1
    if (( SIM_QWEN3_GUEST_DECODE_STEPS > 1 )); then
      if ! rg -q "\\[w4_guest\\] stage qwen3_w5_memory_prefix_cache_kv_loaded node=${idx} step=[1-9][0-9]* previous_step=[0-9]+ .* status=ok" "$log_file"; then
        assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_kv_state_resolve local=node${idx} kv_step=[0-9]+ key=kvcache/qwen3[-.0-9a-z]*(/scope/[0-9a-f]{16})?/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ validation=object_ref_metadata source=obmm_object_view backing=(obmm_shmem|ub_ssd_gsva) metadata=lingqu_object_service target=(mapped_view|local_backend_read_buffer) status=ok" "$node_id model range kv state resolve" || return 1
      fi
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        if (( idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
          assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_history_wait step=[0-9]+ object_key=qwen3/session/[^/]+/tokens/history owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=[0-9]+ history_tokens=[1-9][0-9]* bytes=[1-9][0-9]* checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram history wait" || return 1
          assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_state_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/engram/state owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 history_tokens=[1-9][0-9]* selected_token=[0-9]+ history_checksum=0x[0-9a-f]+ blocked=[0-9]+ fallback=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ bytes=128 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram state wait" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_resolved step=[1-9][0-9]* previous_step=[0-9]+ selected_token=[0-9]+ history_tokens=[1-9][0-9]* history_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ target=next_round_input status=ok" "$node_id qwen3 engram state resolved" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_prompt_tokens_from_history tokens=[1-9][0-9]* source=engram_history_object target=uapi_segment status=ok" "$node_id qwen3 prompt tokens from history" || return 1
        else
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_skip step=[1-9][0-9]* local=${node_id} reason=range_worker_stateless status=ok" "$node_id qwen3 engram state skip" || return 1
          if (( idx == 1 )); then
            assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_prompt_tokens_extended base_tokens=[1-9][0-9]* append_tokens=1 tokens=[1-9][0-9]* source=runtime_token_input target=uapi_segment status=ok" "$node_id qwen3 runtime token input prompt extension" || return 1
          fi
        fi
      fi
    fi
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]] && (( idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
      assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_candidates_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk owner=node${engram_candidates_owner_node} version=1 candidate_count=[1-9][0-9]* bytes=256 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram candidates wait" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_token_select local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ history_tokens=[1-9][0-9]* raw_token=[0-9]+ runner_up=[0-9]+ selected_token=[0-9]+ candidate_count=[1-9][0-9]* candidate2=[0-9]+ candidate3=[0-9]+ blocked=[0-9]+ fallback=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ no_repeat_ngram_size=[0-9]+ repetition_penalty_milli=[0-9]+ history_window=[0-9]+ candidate_checksum=0x[0-9a-f]+ source=guest_policy status=ok" "$node_id qwen3 engram token select" || return 1
      assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_decision_publish local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ objects=3 history_tokens=[1-9][0-9]* selected_token=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ fallback=[0-9]+ blocked=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ history_key=qwen3/session/[^/]+/tokens/history history_version=[0-9]+ selected_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected state_key=qwen3/session/[^/]+/step/[0-9]+/engram/state history_checksum=0x[0-9a-f]+ selected_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram decision publish" || return 1
    fi
    if (( idx == terminal_publish_node )); then
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_candidates_publish local=node${engram_candidates_owner_node} step=[0-9]+ candidate_count=[1-9][0-9]* candidates_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk candidates_version=1 candidates_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram candidates publish" || return 1
        assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage qwen3_engram_selected_token_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 bytes=64 token=[0-9]+ checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram selected token wait" || return 1
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_selected_writeback local=node8 step=[0-9]+ selected_token=[0-9]+ source=engram_selected_object target=terminal_token_result status=ok" "$node_id qwen3 engram selected writeback" || return 1
      fi
      assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_terminal_token_result_publish local=node${engram_candidates_owner_node} target=node[0-9]+ step=[0-9]+ token=[0-9]+ runner_up=[0-9]+ margin_milli=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ piece_word0=0x[0-9a-f]+ piece_word1=0x[0-9a-f]+ object_key=tokens/qwen3[-.0-9a-z]*(/scope/[0-9a-f]{16})?/decode-step[0-9]+ offset=0x[0-9a-f]+ bytes=64 checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=(obmm_spsc|local_pending) status=ok" "$node_id qwen3 terminal token result publish" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_forward_only object=range_hidden publish=0 resolve_remote=0 compute=0 storage=obmm_object metadata=db status=ok" "$node_id qwen3 range-only flow" || return 1
    if (( idx == SIM_W5_CLUSTER_NODE_COUNT )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_logits_sampling_table entries=[12] entry_words=(20|45) table_bytes=(160|360|720) vocab=[1-9][0-9]* sampled_distinct=[12] logits_checksum_nonzero=[12] text_checksum_nonzero=[12] real_logits=[01] status=ok" "$node_id qwen3 logits sampling table" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_token_text_table entries=[12] entry_words=8 table_bytes=(64|128) total_bytes=[1-9][0-9]* piece_bytes=9 policy_kind=2 policy_hash=0x[0-9a-f]+ packed_matches=[12] checksum_matches=[12] boundary_first=1 boundary_last=1 status=ok" "$node_id model token text table" || return 1
    else
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_logits_sampling_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id qwen3 logits sampling table skipped" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_token_text_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id model token text table skipped" || return 1
    fi
  elif is_deepseek_v4_flash_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    local expected_kv_restores=$((SIM_QWEN3_GUEST_DECODE_STEPS - 1))

    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_compute_contract node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=43 hidden_bytes=[1-9][0-9]* source=dispatch_task output=completion status=ok" "$node_id DeepSeek range compute contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_range_runtime_forward node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=$SIM_W5_CLUSTER_NODE_COUNT total_layers=43 hidden_bytes=[1-9][0-9]* input_checksum=0x[0-9a-f]+ output_checksum=0x[0-9a-f]+ range_checksum=0x[0-9a-f]+ real_layers=[1-9][0-9]* .*kv_payload_bytes=[1-9][0-9]* kv_payload_checksum=0x[0-9a-f]+ source=runtime_forward output=metadata status=ok" "$node_id DeepSeek range runtime forward" || return 1
    assert_log_count "$log_file" "\\[w4_guest\\] stage uapi_model_range_runtime_forward node=$((idx - 1)) .*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id DeepSeek range forward per step" || return 1
    if (( idx > 1 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage deepseek_v4_flash_runtime_input_loaded node=${idx} step=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) producer=node$((idx - 1)) kind=[1-9][0-9]* bytes=[1-9][0-9]* checksum=0x[0-9a-f]+ source=mem_service target=uapi_object_ref transport=gsva materialize=uapi_segment status=ok" "$node_id DeepSeek GSVA runtime input" || return 1
    fi
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_forward_runtime_output_publish local=node${idx} step=[0-9]+ .*layers=\\[[0-9]+,[0-9]+\\) .*status=ok" "$node_id DeepSeek runtime output publish" || return 1
    assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_kv_state_publish local=node${idx} step=[0-9]+ key=kvcache/deepseek-v4-flash(/scope/[0-9a-f]{16})?/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ .*status=ok" "$node_id DeepSeek KV publish" || return 1
    assert_log_count "$log_file" "\\[(w4_guest|mem_service)\\] stage model_range_kv_state_publish local=node${idx} .*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id DeepSeek KV publish per step" || return 1
    assert_log_count "$log_file" "\\[w4_guest\\] stage deepseek_v4_flash_layer_kv_restored node=${idx} step=[1-9][0-9]* previous_step=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ source=mem_service target=uapi_object_ref materialize=object_ref status=ok" "$expected_kv_restores" "$node_id DeepSeek KV restore per decode continuation" || return 1
    if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "deepseek-v4-flash-simpler" ||
          "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "deepseek_v4_flash_simpler" ||
          "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "deepseek-v4-flash-official" ||
          "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "deepseek_v4_flash_official" ]]; then
      assert_log_count "$qemu_log" "deepseek-v4-flash-real-range-runtime: engine=simpler node=$((idx - 1)) nodes=$SIM_W5_CLUSTER_NODE_COUNT layers=\\[[0-9]+,[0-9]+\\) terminal_owner=[01] step=[0-9]+ history_tokens=[1-9][0-9]* executed_tokens=[1-9][0-9]* position=[0-9]+ hidden_bytes=[1-9][0-9]* kv_bytes=[1-9][0-9]* routed_layers=[1-9][0-9]* routed_expert_bytes=[1-9][0-9]* route_checksum=0x[0-9a-f]*[1-9a-f][0-9a-f]* .*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id DeepSeek real GGUF MoE execution per step" || return 1
    fi
    if (( idx == terminal_publish_node )); then
      assert_log_has "$log_file" "\\[(w4_guest|mem_service)\\] stage model_terminal_token_result_publish local=node${idx} target=node1 step=0 token=[0-9]+ runner_up=[0-9]+ .*object_key=tokens/deepseek-v4-flash(/scope/[0-9a-f]{16})?/decode-step0 .*status=ok notification=(delivered|backpressured) publisher=terminal_node" "$node_id DeepSeek terminal token object" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage deepseek_v4_flash_first_token node=${idx} step=0 token=[0-9]+ runner_up=[0-9]+ logits_checksum=0x[0-9a-f]+ source=terminal_logits target=stream_output status=ok" "$node_id DeepSeek first token" || return 1
      assert_log_count "$log_file" "\\[w4_guest\\] stage deepseek_v4_flash_stream_token node=${idx} step=[0-9]+ .*status=ok" "$SIM_QWEN3_GUEST_DECODE_STEPS" "$node_id DeepSeek streamed token per step" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_logits_sampling_table entries=1 .*status=ok" "$node_id DeepSeek logits sampling table" || return 1
    else
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_model_logits_sampling_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id DeepSeek logits sampling skipped" || return 1
    fi
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] completion_sources chipbackend=[1-9][0-9]* shmem=[2-9][0-9]* dfs=[2-9][0-9]* db=[2-9][0-9]* block=[2-9][0-9]* guest_uapi=[0-9]+" "$node_id completion source coverage" || return 1
  if is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[w4_guest\\] completion_status success=[1-9][0-9]* retryable=[0-9]+ fatal=0" "$node_id completion status" || return 1
  else
    assert_log_has "$log_file" "\\[w4_guest\\] completion_status success=15 retryable=0 fatal=0" "$node_id completion status" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared" "$node_id uapi kvcache shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=8192 puts=1 gets=1 source=shmem_service role=legacy_kvcache_payload" "$node_id uapi kvcache boundary shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-0 writes=1 reads=1 source=block_service" "$node_id uapi kvcache block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-1 writes=1 reads=1 source=block_service role=aux_block_boundary" "$node_id uapi kvcache aux block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service" "$node_id uapi kvcache db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service role=aux_block" "$node_id uapi kvcache aux db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] assessment service_coverage=5/5 dispatch_path=ubc_entity_chipbackend kvcache_shmem_segment=[0-9]+ kvcache_block=w4-${node_id}-block-0 kvcache_db_key=block/w4-${node_id}-block-0 kvcache_db_bytes=[1-9][0-9]* complete=true" "$node_id service coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] dispatch path=ubc_entity_chipbackend" "$node_id chipbackend dispatch marker" || return 1
  assert_log_absent "$log_file" "observer_metadata_only" "$node_id no observer-only path" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
  assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
  return 0
}

validate_serving_request_node_log() {
  local node_id="$1"
  local log_file="$2"
  local expected_requests="${SIM_W5_SERVING_REQUEST_COUNT:-0}"
  local expected_passes="${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS}"

  if [[ "$SIM_W5_SERVING_INGRESS" == "nodeA" && "$node_id" != "nodeA" ]]; then
    assert_log_count "$log_file" "\\[w4guest8:initramfs\\] serving_entry worker_received .* role=${node_id} " "$expected_requests" "$node_id W5 serving worker requests" || return 1
  else
    assert_log_count "$log_file" "\\[w4guest8:initramfs\\] serving_entry request_start .* role=${node_id} " "$expected_requests" "$node_id W5 serving request starts" || return 1
    if [[ "$SIM_W5_SERVING_QUEUE" != "1" ]]; then
      assert_log_has "$log_file" "\\[w4guest8:initramfs\\] serving_entry completed requests=${expected_requests} role=${node_id}" "$node_id W5 serving entry completion" || return 1
    fi
  fi
  assert_log_count "$log_file" "\\[w4guest8:initramfs\\] serving_entry request_done .* role=${node_id}" "$expected_requests" "$node_id W5 serving request completions" || return 1
  assert_log_count "$log_file" "\\[w4_guest\\] pass" "$expected_passes" "$node_id W5 serving decode passes" || return 1
  if (( expected_requests > 1 )); then
    assert_log_has "$log_file" "\\[ub_obmm_pool\\] import_pa_bias=[1-9][0-9]*MB bytes=0x[0-9a-f]+" "$node_id W5 serving second-request import PA bias" || return 1
  fi
  assert_log_absent "$log_file" "\\[w4_guest\\] fail|\\[w4guest8:initramfs\\] FAIL:|\\[ub_obmm_pool\\] import failed|ret=-EEXIST|File exists" "$node_id W5 serving failures absent" || return 1
  return 0
}

run_w4_app() {
  local decode_step="$1"
  local node_id guest_log rc expected_passes
  typeset -A START_LINES

  expected_passes="${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS}"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    START_LINES[$node_id]=0
    trace "wait w4 guest step=$decode_step on $node_id log=$guest_log"
  done

  rc=0
  wait_for_all_logs_pass_or_fail_since "^\\[w4_guest\\] pass\\r?$" "$FATAL_GUEST_PATTERN" \
    "$((APP_WAIT_SECS * expected_passes))" \
    "$expected_passes" || rc=$?
  if [[ "$rc" != "0" ]]; then
    trace "FAIL: w4 guest did not pass on all nodes rc=$rc"
    return 1
  fi

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    if [[ -n "$SIM_W5_SERVING_REQUESTS_FILE" ]]; then
      validate_serving_request_node_log "$node_id" "$guest_log" || return 1
    else
      validate_node_log "$node_id" "$guest_log" || return 1
    fi
  done
  assert_no_fatal_runtime_logs || return 1
  return 0
}

emit_w4_run_summary() {
  local summary_tmp="$RUN_SUMMARY_FILE.tmp"
  local line
  local summary_parser="$SCRIPT_DIR/w4_guest_run_summary.py"
  local expected_steps="${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS}"

  if [[ -n "$SIM_UAPI_W5_PROFILE" && -f "$SCRIPT_DIR/w5_inference_cluster_summary.py" ]]; then
    summary_parser="$SCRIPT_DIR/w5_inference_cluster_summary.py"
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    trace "summary: unavailable reason=python3_not_found"
    return 1
  fi

  if ! python3 "$summary_parser" \
    "$RUN_DIR" "$expected_steps" "${NODE_IDS[@]}" > "$summary_tmp"; then
    rm -f "$summary_tmp"
    trace "summary: unavailable reason=summary_parser_failed"
    return 1
  fi

  mv "$summary_tmp" "$RUN_SUMMARY_FILE"
  while IFS= read -r line; do
    trace "$line"
  done < "$RUN_SUMMARY_FILE"
  trace "summary_file: $RUN_SUMMARY_FILE"
  return 0
}

prepare_environment() {
  local guest_log node_id control_log env_file launch_rc

  rm -rf "$RUN_DIR" "$RUN_SUMMARY_FILE"
  mkdir -p "$RUN_DIR" "$OUT_DIR"
  : > "$TRACE_FILE"
  env_file="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
  RUN_ENV_FILE="$env_file"
  control_log="$RUN_DIR/control.log"
  validate_qwen3_weights_path || return 1
  qwen3_dense_apply_config_env
  validate_qwen3_runtime_object_view_source || return 1
  validate_w5_profile_runtime || return 1
  resolve_w5_serving_requests_config || return 1
  validate_qwen3_engram_context_refs || return 1
  if is_model_range_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    if [[ -z "$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS" ]]; then
      SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$((APP_WAIT_SECS * 1000))"
    fi
    if [[ -z "$SIM_QWEN3_RUNTIME_RANGE_WAIT_MS" ]]; then
      SIM_QWEN3_RUNTIME_RANGE_WAIT_MS="$((APP_WAIT_SECS * ${SIM_W5_SERVING_DECODE_STEPS_TOTAL:-$SIM_QWEN3_GUEST_DECODE_STEPS} * 1000))"
    fi
    trace "prepare: model range decode round barrier timeout ms=$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS"
    trace "prepare: model range runtime wait timeout ms=$SIM_QWEN3_RUNTIME_RANGE_WAIT_MS"
  fi
  if is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    trace "prepare: qwen3 dense profile=$SIM_UAPI_W4_CHIPBACKEND_PROFILE model_id=${SIM_QWEN3_DENSE_MODEL_ID:-} model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-} layers=${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-} hidden_range_bytes=${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-} decode_hidden_bytes=${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-} tp_nodes=${SIM_QWEN3_DENSE_TP_NODES:-} node_count=$SIM_W5_CLUSTER_NODE_COUNT"
    trace "prepare: qwen3 decode round barrier timeout ms=$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS"
    trace "prepare: model runtime range wait timeout ms=$SIM_QWEN3_RUNTIME_RANGE_WAIT_MS"
  fi
  if [[ "$SIM_W5_GUEST_ENGINE" == "openEuler" ]]; then
    build_w4_openEuler_initramfs
  else
    build_w4_initramfs
  fi
  trace "prepare: guest engine=$SIM_W5_GUEST_ENGINE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  set +e
  ENV_FILE="$env_file" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" QEMU_MEM="$QEMU_MEM" UB_SIM_PORT_NUM="$PORT_NUM" \
    SIM_W5_GUEST_ENGINE="$SIM_W5_GUEST_ENGINE" SIM_W5_OE_DISK_IMAGE="${SIM_W5_OE_DISK_IMAGE:-}" \
    INITRAMFS_IMAGE="$RUN_INITRAMFS_IMAGE" RDINIT="/bin/run_app" \
    UB_FM_SHARED_DIR="$UB_FM_SHARED_DIR" \
    SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
    SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST="$SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST" \
    SIM_UAPI_W5_PROFILE="$SIM_UAPI_W5_PROFILE" \
    SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
    SIM_W5_RUN_ID="$RUN_ID_BASE" \
    SIM_QWEN3_DENSE_MODEL_ID="${SIM_QWEN3_DENSE_MODEL_ID:-}" \
    SIM_QWEN3_DENSE_MODEL_KEY="${SIM_QWEN3_DENSE_MODEL_KEY:-}" \
    SIM_QWEN3_DENSE_WEIGHTS_PATH="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" \
    SIM_QWEN3_DENSE_VOCAB_SIZE="${SIM_QWEN3_DENSE_VOCAB_SIZE:-}" \
    SIM_QWEN3_DENSE_HIDDEN_SIZE="${SIM_QWEN3_DENSE_HIDDEN_SIZE:-}" \
    SIM_QWEN3_DENSE_INTERMEDIATE_SIZE="${SIM_QWEN3_DENSE_INTERMEDIATE_SIZE:-}" \
    SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS="${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-}" \
    SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS="${SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS:-}" \
    SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS="${SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS:-}" \
    SIM_QWEN3_DENSE_HEAD_DIM="${SIM_QWEN3_DENSE_HEAD_DIM:-}" \
    SIM_QWEN3_DENSE_PREFILL_TOKENS="${SIM_QWEN3_DENSE_PREFILL_TOKENS:-}" \
    SIM_QWEN3_DENSE_DECODE_TOKENS="${SIM_QWEN3_DENSE_DECODE_TOKENS:-}" \
    SIM_QWEN3_DENSE_TP_NODES="${SIM_QWEN3_DENSE_TP_NODES:-}" \
    SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES="${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-}" \
    SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES="${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}" \
    SIM_QWEN3_DENSE_KV_STATE_BYTES="${SIM_QWEN3_DENSE_KV_STATE_BYTES:-}" \
    SIM_QWEN3_DENSE_WEIGHTS_PATH="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" \
    SIM_QWEN3_SAMPLER_TOP_K="$SIM_QWEN3_SAMPLER_TOP_K" \
    SIM_QWEN3_SAMPLER_TOP_P_MILLI="$SIM_QWEN3_SAMPLER_TOP_P_MILLI" \
    SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI="$SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI" \
    SIM_QWEN3_SAMPLER_SEED="$SIM_QWEN3_SAMPLER_SEED" \
    SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS" \
    SIM_QWEN3_RUNTIME_RANGE_WAIT_MS="$SIM_QWEN3_RUNTIME_RANGE_WAIT_MS" \
    SIM_QWEN3_GUEST_ENGRAM_MODE="$SIM_QWEN3_GUEST_ENGRAM_MODE" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS" \
    SIM_QWEN3_GUEST_ENGRAM_STATE_REF="$SIM_QWEN3_GUEST_ENGRAM_STATE_REF" \
    SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF="$SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF" \
    SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR" \
    SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT" \
    SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST="$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT_GUEST" \
    SIM_W5_FLASH_WEIGHT_CATALOG="$SIM_W5_FLASH_WEIGHT_CATALOG" \
    SIM_W5_FLASH_WEIGHT_CATALOG_GUEST="$SIM_W5_FLASH_WEIGHT_CATALOG_GUEST" \
    SIM_W5_MEMORY_SERVICE="$SIM_W5_MEMORY_SERVICE" \
    SIM_W5_SERVING_REQUEST_ID="$SIM_W5_SERVING_REQUEST_ID" \
    SIM_W5_SERVING_REQUESTS_FILE="$SIM_W5_SERVING_REQUESTS_FILE" \
    SIM_W5_SERVING_REQUESTS_FILE_GUEST="$SIM_W5_SERVING_REQUESTS_FILE_GUEST" \
    SIM_W5_SERVING_REQUEST_COUNT="$SIM_W5_SERVING_REQUEST_COUNT" \
    SIM_W5_SERVING_DECODE_STEPS_TOTAL="$SIM_W5_SERVING_DECODE_STEPS_TOTAL" \
    SIM_W5_SERVING_QUEUE="$SIM_W5_SERVING_QUEUE" \
    SIM_W5_SERVING_INGRESS="$SIM_W5_SERVING_INGRESS" \
    SIM_W5_TEST_MEMORY_DECISION_STORE="$SIM_W5_TEST_MEMORY_DECISION_STORE" \
    SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE="$SIM_W5_TEST_MEMORY_SHORTPATH_LOOKUP_MODE" \
    SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND="$SIM_W5_TEST_MEMORY_BOUNDARY_LOOKUP_BACKEND" \
    SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID" \
    SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_SUPPORT_ID" \
    SIM_W5_TEST_MEMORY_SHORTPATH_ACTION="$SIM_W5_TEST_MEMORY_SHORTPATH_ACTION" \
    SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_ID" \
    SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START="$SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_START" \
    SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END="$SIM_W5_TEST_MEMORY_SHORTPATH_TARGET_LAYER_END" \
    SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_KIND" \
    SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_CHECKSUM" \
    SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF="$SIM_W5_TEST_MEMORY_SHORTPATH_ARTIFACT_REF" \
    SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF="$SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_REF" \
    SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT="$SIM_W5_TEST_MEMORY_BOUNDARY_REGISTRY_COUNT" \
    SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_START" \
    SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_LAYER_END" \
    SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION="$SIM_W5_TEST_MEMORY_SHORTPATH_PRODUCER_POSITION" \
    SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM="$SIM_W5_TEST_MEMORY_SHORTPATH_PROOF_CHECKSUM" \
    SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE="$SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE" \
	    SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_COUNT" \
	    SIM_W5_TEST_MEMORY_SHORTPATH_STREAM="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM" \
	    SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH="$SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH_GUEST" \
	    SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_COUNT" \
	    SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH="$SIM_W5_TEST_MEMORY_SHORTPATH_KV_STREAM_PATH_GUEST" \
	    SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID="$SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID" \
    SIM_W5_TEST_MEMORY_PREFETCH_SCOPE="$SIM_W5_TEST_MEMORY_PREFETCH_SCOPE" \
    SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX="$SIM_W5_TEST_MEMORY_PREFETCH_TARGET_STEP_INDEX" \
    SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFETCH_CHECKSUM" \
    SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_IDS" \
    SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_CHECKSUMS" \
    SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS="$SIM_W5_TEST_MEMORY_PREFETCH_ARTIFACT_REFS" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ACTION" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_ID" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_CHECKSUM" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_ARTIFACT_REF" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_PROOF_CHECKSUM" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_MATCHED_TOKENS" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_REPLAY_SUFFIX_TOKENS" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_COUNT" \
    SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH="$SIM_W5_TEST_MEMORY_PREFIX_CACHE_KV_STREAM_PATH_GUEST" \
    SIM_W5_TEST_MEMORY_GSVA_KV="$SIM_W5_TEST_MEMORY_GSVA_KV" \
    SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH="$SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH" \
    SIM_ENGRAM_SIMT_ARTIFACT_DIR="${SIM_ENGRAM_SIMT_ARTIFACT_DIR:-}" \
    SIM_ENGRAM_SIMT_SELECTED_SYMBOL="${SIM_ENGRAM_SIMT_SELECTED_SYMBOL:-}" \
    SIM_ENGRAM_SIMT_SELECTED_CASE="${SIM_ENGRAM_SIMT_SELECTED_CASE:-}" \
    SIM_ENGRAM_SIMT_BINARY_PATH="${SIM_ENGRAM_SIMT_BINARY_PATH:-}" \
    SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH="${SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH:-}" \
    "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null
  launch_rc=$?
  set -e
  if (( launch_rc != 0 )); then
    trace "FAIL: headless launch/preflight failed rc=$launch_rc"
    if [[ -f "$control_log" ]]; then
      trace "headless control log tail follows path=$control_log"
      tail -n 80 "$control_log" | sed 's/^/[w4guest8] control: /' | tee -a "$TRACE_FILE" >&2
    fi
    return 1
  fi
  if [[ ! -f "$env_file" ]]; then
    trace "FAIL: headless env file was not created path=$env_file"
    if [[ -f "$control_log" ]]; then
      trace "headless control log tail follows path=$control_log"
      tail -n 80 "$control_log" | sed 's/^/[w4guest8] control: /' | tee -a "$TRACE_FILE" >&2
    fi
    return 1
  fi
  source "$env_file"
  append_run_artifact_cleanup
  trace_run_artifact_paths

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    if [[ "$SIM_W5_SERVING_QUEUE" == "1" ]]; then
      trace "wait W5 serving entry ready: $node_id"
      local serving_ready_mode="serial-line"
      if [[ "$SIM_W5_SERVING_INGRESS" == "nodeA" && "$node_id" != "nodeA" ]]; then
        serving_ready_mode="nodeA-worker"
      fi
      if ! wait_for_log_pattern "$guest_log" "\\[w4guest8:initramfs\\] serving_entry ready mode=$serving_ready_mode role=$node_id entry=nodeA" "$BOOT_WAIT_SECS"; then
        trace "FAIL: W5 serving entry ready timeout for $node_id"
        return 1
      fi
      continue
    fi
    trace "wait initramfs runner gate: $node_id"
    if ! wait_for_log_pattern "$guest_log" "\\[w4guest8:initramfs\\] start step=0 $node_id" "$BOOT_WAIT_SECS"; then
      trace "FAIL: initramfs runner gate timeout for $node_id"
      return 1
    fi
  done
  if [[ "$SIM_W5_SERVING_QUEUE" == "1" ]]; then
    trace "W5 serving entry ready for all $SIM_W5_CLUSTER_NODE_COUNT nodes env_file=$env_file"
  else
    trace "initramfs runner gate ok for all $SIM_W5_CLUSTER_NODE_COUNT nodes"
  fi
  return 0
}

main() {
  local exit_code=1
  local step

  if [[ "${1:-}" == "--validate-w5-artifact-sizes-only" ]]; then
    shift
    run_w5_artifact_size_validation_cli "$@"
    exit "$?"
  fi

  if ! prepare_environment; then
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if [[ "$SIM_W5_SERVING_QUEUE" == "1" ]]; then
    trace "PASS: W5 serving queue ready env_file=$RUN_ENV_FILE"
    if [[ -n "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" ]]; then
      trace "submit W5 serving requests file=$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE env_file=$RUN_ENV_FILE"
      if [[ -z "$SIM_W5_SERVING_REQUEST_COUNT" ]]; then
        SIM_W5_SERVING_REQUEST_COUNT="$("$SCRIPT_DIR/w5_serving_entry.py" --requests "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" --print-request-count)"
      fi
      if [[ -z "$SIM_W5_SERVING_DECODE_STEPS_TOTAL" ]]; then
        SIM_W5_SERVING_DECODE_STEPS_TOTAL="$("$SCRIPT_DIR/w5_serving_entry.py" --requests "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" --print-total-decode-steps)"
      fi
      local submit_args=(
        --env-file "$RUN_ENV_FILE"
        --requests "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE"
        --wait-done
        --wait-timeout "$SIM_W5_SERVING_SUBMIT_WAIT_SECS"
      )
      if [[ "$SIM_W5_SERVING_INGRESS" == "nodeA" ]]; then
        submit_args+=(--fanout nodeA --wait-targets cluster)
      fi
      if ! "$SCRIPT_DIR/run_w5_serving_submit.sh" \
          "${submit_args[@]}"; then
        trace "FAIL: W5 serving submit failed file=$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE"
        [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
        exit 1
      fi
      for node_id in "${NODE_IDS[@]}"; do
        if ! validate_serving_request_node_log "$node_id" "$RUN_DIR/${node_id}_guest.log"; then
          trace "FAIL: W5 serving request validation failed node=$node_id file=$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE"
          [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
          exit 1
        fi
      done
      trace "PASS: W5 serving requests completed file=$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE"
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      echo "W5 serving requests completed"
      echo "$RUN_ENV_FILE"
      exit 0
    fi
    echo "W5 serving queue ready"
    echo "$RUN_ENV_FILE"
    exit 0
  fi

  if ! run_w4_app 0; then
    if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
      trace "FAIL: W5 inference cluster validation failed nodes=$SIM_W5_CLUSTER_NODE_COUNT profile=$SIM_UAPI_W5_PROFILE"
    else
      trace "FAIL: W4 guest resource-backed uapi/chipbackend service coverage validation failed nodes=$SIM_W5_CLUSTER_NODE_COUNT"
    fi
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if true; then
    exit_code=0
    emit_w4_run_summary || true
    if ! validate_w5_engram_context_summary; then
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      exit 1
    fi
    if ! validate_w5_boundary_observation_summary; then
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      exit 1
    fi
    if ! validate_w5_artifact_sizes; then
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      exit 1
    fi
    if ! emit_w5_inference_run_report; then
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      exit 1
    fi
    if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
      trace "PASS: W5 inference cluster nodes=$SIM_W5_CLUSTER_NODE_COUNT profile=$SIM_UAPI_W5_PROFILE"
      echo "$SIM_W5_CLUSTER_NODE_COUNT-node W5 inference cluster validation passed"
    else
      trace "PASS: W4 guest resource-backed uapi/chipbackend service coverage validated nodes=$SIM_W5_CLUSTER_NODE_COUNT"
      echo "$SIM_W5_CLUSTER_NODE_COUNT-node W4 guest validation passed"
    fi
  fi

  [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
  exit "$exit_code"
}

main "$@"
