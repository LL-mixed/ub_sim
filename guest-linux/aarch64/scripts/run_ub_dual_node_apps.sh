#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_app}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}"
RUN_SECS="${RUN_SECS:-180}"
ITERATIONS="${ITERATIONS:-1}"
START_GAP_SECS="${START_GAP_SECS:-3}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
USE_QMP="${USE_QMP:-0}"
APP_SELECTION="${APP_SELECTION:-}"
REMOTE_MEMORY_MODEL_MANIFEST="${REMOTE_MEMORY_MODEL_MANIFEST:-}"
ASYNC_LOAD_MODEL="${ASYNC_LOAD_MODEL:-}"
OBMM_ASYNC_ARGS="${OBMM_ASYNC_ARGS:-}"
APPEND_EXTRA_WAS_SET=0
if [[ -n "${APPEND_EXTRA+x}" ]]; then
  APPEND_EXTRA_WAS_SET=1
fi
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-identity}"
OBMM_GSVA_BASE="${OBMM_GSVA_BASE:-0x700000000000}"
OBMM_GSVA_SIZE="${OBMM_GSVA_SIZE:-0x400000}"
OBMM_GSVA_NODE_COUNT="${OBMM_GSVA_NODE_COUNT:-2}"
COH_TEST_MODE="${COH_TEST_MODE:-write_read}"
COH_TEST_SIZE="${COH_TEST_SIZE:-2097152}"
COH_TEST_ITERS="${COH_TEST_ITERS:-1}"
COH_TEST_TOKEN_VALUE="${COH_TEST_TOKEN_VALUE:-0}"
COH_TEST_GENERATION="${COH_TEST_GENERATION:-1}"
COH_TEST_VERBOSE="${COH_TEST_VERBOSE:-1}"
MEM_SERVICE_OBMM_PROVIDER_GENERATION="${MEM_SERVICE_OBMM_PROVIDER_GENERATION:-101}"
GVA_DIRECT_MODE="${GVA_DIRECT_MODE:-write-read}"
GVA_DIRECT_LOCAL_VA="${GVA_DIRECT_LOCAL_VA:-0x710000000000}"
GVA_DIRECT_HOME_VA="${GVA_DIRECT_HOME_VA:-0x720000000000}"
GVA_DIRECT_SIZE="${GVA_DIRECT_SIZE:-0x400000}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-host_matmul}"
SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
SIMPLER_HOST_VECTOR_MANIFEST="${SIMPLER_HOST_VECTOR_MANIFEST:-/tmp/simpler-host-vector-artifacts/host_vector_manifest.json}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIM_UAPI_SCENARIO_CONFIG="${SIM_UAPI_SCENARIO_CONFIG:-$WORKSPACE_ROOT/scenarios/mvp_4host_single_domain.yaml}"
LINGQU_SHMEM_PTO_ELEMENTS="${LINGQU_SHMEM_PTO_ELEMENTS:-16384}"
LINGQU_SHMEM_PTO_LAYOUT="${LINGQU_SHMEM_PTO_LAYOUT:-nd}"
LINGQU_SHMEM_PTO_GENERATION="${LINGQU_SHMEM_PTO_GENERATION:-101}"
LINGQU_SHMEM_PTO_TOKEN_VALUE="${LINGQU_SHMEM_PTO_TOKEN_VALUE:-0}"
LINGQU_SHMEM_PTO_TIMEOUT_MS="${LINGQU_SHMEM_PTO_TIMEOUT_MS:-120000}"
LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT="${LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT:-}"
LINGQU_SHMEM_PTO_NODEA_CNA="${LINGQU_SHMEM_PTO_NODEA_CNA:-0xf001}"
LINGQU_SHMEM_PTO_NODEB_CNA="${LINGQU_SHMEM_PTO_NODEB_CNA:-0xf002}"
LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS="${LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS:-0}"
LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS="${LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS:-1000000000}"
LINGQU_SHMEM_PTO_CANCEL_AFTER_MS="${LINGQU_SHMEM_PTO_CANCEL_AFTER_MS:-0}"
LINGQU_SHMEM_PTO_RESET_ON_PENDING="${LINGQU_SHMEM_PTO_RESET_ON_PENDING:-0}"
LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION="${LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION:-0}"
LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION="${LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION:-0}"
LINGQU_SHMEM_PTO_EXPECT="${LINGQU_SHMEM_PTO_EXPECT:-success}"
LINGQU_SHMEM_PTO_FAULT_CASE="${LINGQU_SHMEM_PTO_FAULT_CASE:-none}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
QMP_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}/qmp"
MIN_PASS_RATE_PERCENT="${MIN_PASS_RATE_PERCENT:-100}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/apps_report.latest.txt}"
MAX_RUNTIME="${MAX_RUNTIME:-300}"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_${RANDOM}}"
INTERACTIVE_AFTER_PASS=0
MAIN_PID=$$

usage() {
  cat <<'USAGE'
Usage: run_ub_dual_node_apps.sh [options]

Options:
  --app NAME          App to validate. Repeat or pass comma-separated names.
                      Names: chat, rpc, tcp_each_server, udma, obmm_pool,
                      obmm_dataplane_microbench, obmm_async_coroutine,
                      obmm_import_stress, obmm_gsva,
                      obmm_coh_test, gva_direct, gsva_query, npu_test, ssd_test,
                      ssd_gsva_test, mem_service,
                      mem_service_obmm_provider_conformance, llm_infer,
                      llm_infer_mem_service, pretraining_client_mem_service,
                      lingqu_shmem_pto_direct.
                      Default: chat,rpc,tcp_each_server.
  --run-id ID        Stable run id used for log/report names.
  --run-secs SECS    Per-app pass/fail wait timeout.
  --iterations N     Number of dual-node iterations.
  --max-runtime SECS Global watchdog timeout.
  --report-file PATH Validation report output path.
  --kernel-image PATH Arm64 guest kernel image.
  --initramfs-image PATH Guest initramfs containing selected apps.
  --interactive-after-pass
                     Keep validated guests at their shells until terminated.
  --append-extra STR Extra kernel cmdline tokens to append.
  --remote-memory-model-manifest PATH
                      Canonical QEMU remote-memory model manifest.
  --async-load-model SPEC
                      Canonical v2 event/upcall capacity spec.
  --obmm-async-args STR
                      Arguments for obmm_async_coroutine.
  --pto-manifest PATH Simpler host-vector manifest for PTO UB_GM dispatch.
  --pto-scenario PATH Two-host simulator scenario used by the Rust bridge.
  --pto-artifact-fingerprint N
                      Callable fingerprint computed from that manifest.
  --pto-layout LAYOUT PTO UB_GM layout: nd, tail, cross-page, or unaligned.
  --pto-elements N    Host-vector element count required by that layout.
  --pto-generation N  OBMM bootstrap generation.
  --pto-token-value N OBMM import token value.
  --pto-timeout-ms N  Producer verification and dispatch timeout.
  --pto-nodea-cna N   QEMU PTO device CNA for nodeA.
  --pto-nodeb-cna N   QEMU PTO device CNA and guest requester CNA for nodeB.
  --pto-authorization-delay-ns N
                      Per-memref QEMU authorization delay; zero is sync.
  --pto-authorization-timeout-ns N
                      Dispatch-wide authorization timeout.
  --pto-cancel-after-ms N
                      Cancel the pending dispatch after N guest milliseconds.
  --pto-reset-on-pending 0|1
                      Reset nodeB through QMP after authorization suspends.
  --pto-inject-duplicate-completion 0|1
                      Inject a duplicate authorization timer completion.
  --pto-inject-late-completion 0|1
                      Inject a completion after cancel/reset cleanup.
  --pto-expect OUTCOME
                      Expected PTO result: success, authorization-timeout,
                      authorization-cancelled, bad-memref, or access-denied.
  --pto-fault-case CASE
                      Test-only dispatch fault: none, bad-mapping-ref,
                      stale-mapping, released-import, retired-segment,
                      wrong-requester, oob,
                      shape-stride-oob, cross-segment, address-overflow,
                      role-access-mismatch,
                      tstore-on-read, or tload-on-write.
  --use-qmp          Start guests paused and resume them through QMP.
  --use-prebuilt-qemu
                     Require the QEMU binary already built by the wrapper.
  -h, --help         Show this help.
USAGE
}

append_app_selection() {
  local app="$1"
  local flag=""

  case "$app" in
    chat)
      flag="linqu_ub_chat=1"
      ;;
    rpc)
      flag="linqu_ub_rpc=1"
      ;;
    tcp|tcp_each_server)
      flag="linqu_ub_tcp_each_server=1"
      ;;
    udma)
      flag="linqu_ub_udma=1"
      ;;
    obmm|obmm_pool)
      flag="linqu_obmm_pool=1"
      ;;
    obmm_dataplane_microbench)
      flag="linqu_obmm_dataplane_microbench=1"
      ;;
    obmm_async_coroutine)
      flag="linqu_obmm_async_coroutine=1"
      ;;
    obmm_import_stress)
      flag="linqu_obmm_import_stress=1"
      ;;
    obmm_gsva)
      flag="linqu_obmm_gsva=1"
      ;;
    obmm_coh_test)
      flag="linqu_obmm_coh_test=1"
      ;;
    mem_service_obmm_provider_conformance)
      flag="linqu_mem_service_obmm_provider_conformance=1"
      ;;
    gva_direct)
      flag="linqu_gva_direct=1"
      ;;
    gsva_query)
      flag="linqu_gsva_query=1"
      ;;
    npu_test)
      flag="linqu_npu_test=1"
      ;;
    ssd_test)
      flag="linqu_ssd_test=1"
      ;;
    ssd_gsva_test)
      flag="linqu_ssd_gsva_test=1"
      ;;
    mem_service)
      flag="linqu_mem_service=1"
      ;;
    llm_infer)
      flag="linqu_llm_infer=1"
      ;;
    llm_infer_mem_service)
      flag="linqu_llm_infer_mem_service=1"
      ;;
    pretraining_client_mem_service)
      flag="linqu_pretraining_client_mem_service=1"
      ;;
    lingqu_shmem_pto_direct)
      flag="linqu_shmem_pto_direct=1"
      ;;
    "")
      return 0
      ;;
    *)
      echo "unknown app selection: $app" >&2
      usage >&2
      exit 2
      ;;
  esac

  if [[ " $APPEND_EXTRA " != *" $flag "* ]]; then
    APPEND_EXTRA="${APPEND_EXTRA} ${flag}"
  fi
}

append_cmdline_if_missing() {
  local token="$1"

  if [[ "$APPEND_EXTRA" != *" $token "* ]] && [[ "$APPEND_EXTRA" != "$token "* ]] &&
    [[ "$APPEND_EXTRA" != *" $token" ]]; then
    APPEND_EXTRA="${APPEND_EXTRA} ${token}"
  fi
}

append_obmm_async_args() {
  local words=(${=OBMM_ASYNC_ARGS})
  local index=1
  local option=""
  local value=""

  while (( index <= ${#words[@]} )); do
    option="${words[$index]}"
    if [[ "$option" == "--verify" ]]; then
      append_cmdline_if_missing "obmm_async_verify=1"
      index=$((index + 1))
      continue
    fi
    if (( index == ${#words[@]} )); then
      echo "OBMM async option requires a value: $option" >&2
      exit 2
    fi
    value="${words[$((index + 1))]}"
    case "$option" in
      --mode) append_cmdline_if_missing "obmm_async_mode=$value" ;;
      --async-load-completion) append_cmdline_if_missing "obmm_async_load_completion=$value" ;;
      --coroutines) append_cmdline_if_missing "obmm_async_coroutines=$value" ;;
      --inflight) append_cmdline_if_missing "obmm_async_inflight=$value" ;;
      --lookahead) append_cmdline_if_missing "obmm_async_lookahead=$value" ;;
      --access-bytes) append_cmdline_if_missing "obmm_async_access_bytes=$value" ;;
      --pattern) append_cmdline_if_missing "obmm_async_pattern=$value" ;;
      --compute-us) append_cmdline_if_missing "obmm_async_compute_us=$value" ;;
      --iterations) append_cmdline_if_missing "obmm_async_iterations=$value" ;;
      --warmup) append_cmdline_if_missing "obmm_async_warmup=$value" ;;
      --min-duration-ms) append_cmdline_if_missing "obmm_async_min_duration_ms=$value" ;;
      --deadline-us) append_cmdline_if_missing "obmm_async_deadline_us=$value" ;;
      --seed) append_cmdline_if_missing "obmm_async_seed=$value" ;;
      --peer-index) append_cmdline_if_missing "obmm_async_peer_index=$value" ;;
      --uffd-case) append_cmdline_if_missing "obmm_uffd_case=$value" ;;
      --worker-threads) append_cmdline_if_missing "obmm_uffd_worker_threads=$value" ;;
      --handler-cpu) append_cmdline_if_missing "obmm_uffd_handler_cpu=$value" ;;
      --pages) append_cmdline_if_missing "obmm_uffd_pages=$value" ;;
      --case) append_cmdline_if_missing "obmm_baseline_case=$value" ;;
      --eval-band) append_cmdline_if_missing "obmm_eval_band=$value" ;;
      --eval-case) append_cmdline_if_missing "obmm_eval_case=$value" ;;
      *)
        echo "unsupported OBMM async option: $option" >&2
        exit 2
        ;;
    esac
    index=$((index + 2))
  done
}

require_pto_unsigned_value() {
  local label="$1"
  local value="$2"

  if ! printf '%s\n' "$value" | grep -Eq '^(0[xX][0-9a-fA-F]+|[0-9]+)$'; then
    echo "$label must be an unsigned decimal or hexadecimal integer" >&2
    exit 2
  fi
}

validate_pto_layout_manifest() {
  local manifest="$1"
  local expected_layout="$2"
  local expected_elements="$3"

  python3 - "$manifest" "$expected_layout" "$expected_elements" <<'PY'
import json
import sys

manifest_path, expected_layout, expected_elements_text = sys.argv[1:]
expected_elements = int(expected_elements_text)
with open(manifest_path, "r", encoding="utf-8") as stream:
    manifest = json.load(stream)
layout = manifest.get("ub_gm_layout")
if not isinstance(layout, dict):
    raise SystemExit("PTO manifest has no ub_gm_layout contract")
actual = {
    "profile": layout.get("profile"),
    "global_rows": layout.get("global_rows"),
    "global_cols": layout.get("global_cols"),
    "tile_rows": layout.get("tile_rows"),
    "tile_cols": layout.get("tile_cols"),
    "logical_elements": layout.get("logical_elements"),
}
expected_geometry = {
    "nd": (128, 128, 128, 128),
    "tail": (128, 127, 128, 128),
    "cross-page": (1, 64, 1, 64),
    "unaligned": (1, 64, 1, 64),
}
geometry = expected_geometry[expected_layout]
expected = {
    "profile": expected_layout,
    "global_rows": geometry[0],
    "global_cols": geometry[1],
    "tile_rows": geometry[2],
    "tile_cols": geometry[3],
    "logical_elements": expected_elements,
}
if actual != expected:
    raise SystemExit(
        "PTO manifest layout mismatch: "
        f"expected={expected!r} actual={actual!r}"
    )
PY
}

validate_lingqu_shmem_pto_config() {
  local cancel_after_ns=0
  local expected_layout_elements=0
  local fault_expected=""
  local value=""

  if [[ ! -f "$SIMPLER_HOST_VECTOR_MANIFEST" ]]; then
    echo "PTO host-vector manifest does not exist: $SIMPLER_HOST_VECTOR_MANIFEST" >&2
    exit 2
  fi
  SIMPLER_HOST_VECTOR_MANIFEST="$(
    cd "$(dirname "$SIMPLER_HOST_VECTOR_MANIFEST")" && pwd
  )/$(basename "$SIMPLER_HOST_VECTOR_MANIFEST")"
  if [[ ! -f "$SIM_UAPI_SCENARIO_CONFIG" ]]; then
    echo "PTO simulator scenario does not exist: $SIM_UAPI_SCENARIO_CONFIG" >&2
    exit 2
  fi
  SIM_UAPI_SCENARIO_CONFIG="$(
    cd "$(dirname "$SIM_UAPI_SCENARIO_CONFIG")" && pwd
  )/$(basename "$SIM_UAPI_SCENARIO_CONFIG")"
  if [[ -z "$LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT" ]]; then
    echo "lingqu_shmem_pto_direct requires --pto-artifact-fingerprint" >&2
    exit 2
  fi

  for value in \
    "elements:$LINGQU_SHMEM_PTO_ELEMENTS" \
    "generation:$LINGQU_SHMEM_PTO_GENERATION" \
    "token-value:$LINGQU_SHMEM_PTO_TOKEN_VALUE" \
    "timeout-ms:$LINGQU_SHMEM_PTO_TIMEOUT_MS" \
    "artifact-fingerprint:$LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT" \
    "nodeA-cna:$LINGQU_SHMEM_PTO_NODEA_CNA" \
    "nodeB-cna:$LINGQU_SHMEM_PTO_NODEB_CNA" \
    "authorization-delay-ns:$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS" \
    "authorization-timeout-ns:$LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS" \
    "cancel-after-ms:$LINGQU_SHMEM_PTO_CANCEL_AFTER_MS" \
    "reset-on-pending:$LINGQU_SHMEM_PTO_RESET_ON_PENDING" \
    "inject-duplicate-completion:$LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION" \
    "inject-late-completion:$LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION"; do
    require_pto_unsigned_value "${value%%:*}" "${value#*:}"
  done
  case "$LINGQU_SHMEM_PTO_LAYOUT" in
    nd)
      expected_layout_elements=16384
      ;;
    tail)
      expected_layout_elements=16256
      ;;
    cross-page|unaligned)
      expected_layout_elements=64
      ;;
    *)
      echo "unsupported PTO layout: $LINGQU_SHMEM_PTO_LAYOUT" >&2
      exit 2
      ;;
  esac
  if (( LINGQU_SHMEM_PTO_ELEMENTS != expected_layout_elements )); then
    echo "PTO layout $LINGQU_SHMEM_PTO_LAYOUT requires exactly $expected_layout_elements elements" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_GENERATION == 0 ||
        LINGQU_SHMEM_PTO_GENERATION > 281474976710655 )); then
    echo "PTO generation must be in 1..281474976710655" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_TOKEN_VALUE > 4294967295 )); then
    echo "PTO token value exceeds uint32" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_TIMEOUT_MS == 0 )); then
    echo "PTO timeout must be nonzero" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS == 0 )); then
    echo "PTO authorization timeout must be nonzero" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_RESET_ON_PENDING > 1 ||
        LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION > 1 ||
        LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION > 1 )); then
    echo "PTO reset and completion injection switches must be 0 or 1" >&2
    exit 2
  fi
  cancel_after_ns=$((LINGQU_SHMEM_PTO_CANCEL_AFTER_MS * 1000000))
  case "$LINGQU_SHMEM_PTO_FAULT_CASE" in
    none)
      ;;
    wrong-requester|tstore-on-read|tload-on-write)
      fault_expected="access-denied"
      ;;
    bad-mapping-ref|stale-mapping|released-import|retired-segment|oob|shape-stride-oob|cross-segment|address-overflow|role-access-mismatch)
      fault_expected="bad-memref"
      ;;
    *)
      echo "unsupported PTO fault case: $LINGQU_SHMEM_PTO_FAULT_CASE" >&2
      exit 2
      ;;
  esac
  case "$LINGQU_SHMEM_PTO_EXPECT" in
    success)
      if (( LINGQU_SHMEM_PTO_CANCEL_AFTER_MS != 0 )) ||
         [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" != "none" ]]; then
        echo "PTO success requires fault-case=none and cancel-after-ms=0" >&2
        exit 2
      fi
      ;;
    authorization-timeout)
      if (( LINGQU_SHMEM_PTO_CANCEL_AFTER_MS != 0 )) ||
         [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" != "none" ]]; then
        echo "authorization-timeout requires fault-case=none and cancel-after-ms=0" >&2
        exit 2
      fi
      if (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS <=
            LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS )); then
        echo "authorization-timeout requires delay-ns greater than timeout-ns" >&2
        exit 2
      fi
      ;;
    authorization-cancelled)
      if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" != "none" ]] ||
         (( LINGQU_SHMEM_PTO_CANCEL_AFTER_MS == 0 ||
            LINGQU_SHMEM_PTO_CANCEL_AFTER_MS >=
              LINGQU_SHMEM_PTO_TIMEOUT_MS )); then
        echo "authorization-cancelled requires fault-case=none and cancel-after-ms in 1..timeout-ms-1" >&2
        exit 2
      fi
      if (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS <= cancel_after_ns ||
            LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS <= cancel_after_ns )); then
        echo "authorization-cancelled requires both QEMU authorization deadlines after the guest cancel point" >&2
        exit 2
      fi
      ;;
    bad-memref|access-denied)
      if [[ -z "$fault_expected" ||
            "$LINGQU_SHMEM_PTO_EXPECT" != "$fault_expected" ]]; then
        echo "PTO fault case $LINGQU_SHMEM_PTO_FAULT_CASE requires expected result ${fault_expected:-success}" >&2
        exit 2
      fi
      if (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS != 0 ||
            LINGQU_SHMEM_PTO_CANCEL_AFTER_MS != 0 ||
            LINGQU_SHMEM_PTO_RESET_ON_PENDING != 0 ||
            LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION != 0 ||
            LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION != 0 )); then
        echo "PTO fault cases require synchronous authorization without lifecycle injections" >&2
        exit 2
      fi
      ;;
    *)
      echo "PTO expected result must be success, authorization-timeout, authorization-cancelled, bad-memref, or access-denied" >&2
      exit 2
      ;;
  esac
  if [[ "$LINGQU_SHMEM_PTO_LAYOUT" != "nd" ]] &&
     { [[ "$LINGQU_SHMEM_PTO_EXPECT" != "success" ]] ||
       [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" != "none" ]]; }; then
    echo "PTO layout $LINGQU_SHMEM_PTO_LAYOUT currently requires success with fault-case=none" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION != 0 )); then
    if [[ "$LINGQU_SHMEM_PTO_EXPECT" != "success" ]] ||
       (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS == 0 )); then
      echo "duplicate completion injection requires delayed successful authorization" >&2
      exit 2
    fi
  fi
  if (( LINGQU_SHMEM_PTO_RESET_ON_PENDING != 0 )); then
    if [[ "$LINGQU_SHMEM_PTO_EXPECT" != "success" ]] ||
       (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS == 0 ||
          LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS >
            LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS ||
          LINGQU_SHMEM_PTO_CANCEL_AFTER_MS != 0 ||
          LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION != 0 )); then
      echo "reset-on-pending requires delayed successful authorization without cancel or duplicate injection" >&2
      exit 2
    fi
    USE_QMP=1
  fi
  if (( LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION != 0 )); then
    if [[ "$LINGQU_SHMEM_PTO_EXPECT" != "authorization-cancelled" ]] &&
       (( LINGQU_SHMEM_PTO_RESET_ON_PENDING == 0 )); then
      echo "late completion injection requires cancellation or reset-on-pending" >&2
      exit 2
    fi
  fi
  if printf '%s\n' "$LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT" |
     grep -Eq '^(0[xX]0+|0+)$'; then
    echo "PTO artifact fingerprint must be nonzero" >&2
    exit 2
  fi
  if (( LINGQU_SHMEM_PTO_NODEA_CNA == 0 ||
        LINGQU_SHMEM_PTO_NODEA_CNA > 0x00ffffff ||
        LINGQU_SHMEM_PTO_NODEB_CNA == 0 ||
        LINGQU_SHMEM_PTO_NODEB_CNA > 0x00ffffff )); then
    echo "PTO device CNA must be in 1..0x00ffffff" >&2
    exit 2
  fi
  validate_pto_layout_manifest \
    "$SIMPLER_HOST_VECTOR_MANIFEST" \
    "$LINGQU_SHMEM_PTO_LAYOUT" \
    "$LINGQU_SHMEM_PTO_ELEMENTS"

  LINGQU_SHMEM_PTO_ELEMENTS=$((LINGQU_SHMEM_PTO_ELEMENTS))
  LINGQU_SHMEM_PTO_GENERATION=$((LINGQU_SHMEM_PTO_GENERATION))
  LINGQU_SHMEM_PTO_TOKEN_VALUE=$((LINGQU_SHMEM_PTO_TOKEN_VALUE))
  LINGQU_SHMEM_PTO_TIMEOUT_MS=$((LINGQU_SHMEM_PTO_TIMEOUT_MS))
  LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS=$((LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS))
  LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS=$((LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS))
  LINGQU_SHMEM_PTO_CANCEL_AFTER_MS=$((LINGQU_SHMEM_PTO_CANCEL_AFTER_MS))
  LINGQU_SHMEM_PTO_RESET_ON_PENDING=$((LINGQU_SHMEM_PTO_RESET_ON_PENDING))
  LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION=$((LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION))
  LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION=$((LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION))
  printf -v LINGQU_SHMEM_PTO_NODEA_CNA '0x%x' \
    "$((LINGQU_SHMEM_PTO_NODEA_CNA))"
  printf -v LINGQU_SHMEM_PTO_NODEB_CNA '0x%x' \
    "$((LINGQU_SHMEM_PTO_NODEB_CNA))"

  append_cmdline_if_missing "linqu_node_count=2"
  append_cmdline_if_missing "lingqu_shmem_pto_layout=$LINGQU_SHMEM_PTO_LAYOUT"
  append_cmdline_if_missing "lingqu_shmem_pto_elements=$LINGQU_SHMEM_PTO_ELEMENTS"
  append_cmdline_if_missing "lingqu_shmem_pto_generation=$LINGQU_SHMEM_PTO_GENERATION"
  append_cmdline_if_missing "lingqu_shmem_pto_token_value=$LINGQU_SHMEM_PTO_TOKEN_VALUE"
  append_cmdline_if_missing "lingqu_shmem_pto_timeout_ms=$LINGQU_SHMEM_PTO_TIMEOUT_MS"
  append_cmdline_if_missing "lingqu_shmem_pto_cancel_after_ms=$LINGQU_SHMEM_PTO_CANCEL_AFTER_MS"
  append_cmdline_if_missing "lingqu_shmem_pto_expect=$LINGQU_SHMEM_PTO_EXPECT"
  append_cmdline_if_missing "lingqu_shmem_pto_fault_case=$LINGQU_SHMEM_PTO_FAULT_CASE"
  append_cmdline_if_missing \
    "lingqu_shmem_pto_artifact_fingerprint=$LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT"
}

apply_app_selection() {
  local selection="$1"
  local app=""

  if [[ -z "$selection" ]]; then
    if [[ "$APPEND_EXTRA_WAS_SET" -eq 1 ]]; then
      return 0
    fi
    selection="chat,rpc,tcp_each_server"
  fi

  for app in ${(s:,:)selection}; do
    append_app_selection "$app"
  done
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --app|--apps)
      if [[ $# -lt 2 ]]; then
        echo "$1 requires a value" >&2
        usage >&2
        exit 2
      fi
      if [[ -z "$APP_SELECTION" ]]; then
        APP_SELECTION="$2"
      else
        APP_SELECTION="${APP_SELECTION},$2"
      fi
      shift 2
      ;;
    --run-id)
      if [[ $# -lt 2 ]]; then
        echo "--run-id requires a value" >&2
        usage >&2
        exit 2
      fi
      RUN_ID="$2"
      shift 2
      ;;
    --run-secs)
      if [[ $# -lt 2 ]]; then
        echo "--run-secs requires a value" >&2
        usage >&2
        exit 2
      fi
      RUN_SECS="$2"
      shift 2
      ;;
    --iterations)
      if [[ $# -lt 2 ]]; then
        echo "--iterations requires a value" >&2
        usage >&2
        exit 2
      fi
      ITERATIONS="$2"
      shift 2
      ;;
    --max-runtime)
      if [[ $# -lt 2 ]]; then
        echo "--max-runtime requires a value" >&2
        usage >&2
        exit 2
      fi
      MAX_RUNTIME="$2"
      shift 2
      ;;
    --report-file)
      if [[ $# -lt 2 ]]; then
        echo "--report-file requires a value" >&2
        usage >&2
        exit 2
      fi
      REPORT_FILE="$2"
      shift 2
      ;;
    --kernel-image)
      if [[ $# -lt 2 ]]; then
        echo "--kernel-image requires a value" >&2
        usage >&2
        exit 2
      fi
      KERNEL_IMAGE="$2"
      shift 2
      ;;
    --initramfs-image)
      if [[ $# -lt 2 ]]; then
        echo "--initramfs-image requires a value" >&2
        usage >&2
        exit 2
      fi
      INITRAMFS_IMAGE="$2"
      shift 2
      ;;
    --append-extra)
      if [[ $# -lt 2 ]]; then
        echo "--append-extra requires a value" >&2
        usage >&2
        exit 2
      fi
      APPEND_EXTRA="${APPEND_EXTRA} $2"
      shift 2
      ;;
    --remote-memory-model-manifest)
      if [[ $# -lt 2 ]]; then
        echo "--remote-memory-model-manifest requires a value" >&2
        exit 2
      fi
      REMOTE_MEMORY_MODEL_MANIFEST="$2"
      shift 2
      ;;
    --async-load-model)
      if [[ $# -lt 2 ]]; then
        echo "--async-load-model requires a value" >&2
        exit 2
      fi
      ASYNC_LOAD_MODEL="$2"
      shift 2
      ;;
    --obmm-async-args)
      if [[ $# -lt 2 ]]; then
        echo "--obmm-async-args requires a value" >&2
        exit 2
      fi
      OBMM_ASYNC_ARGS="$2"
      shift 2
      ;;
    --pto-manifest)
      if [[ $# -lt 2 ]]; then
        echo "--pto-manifest requires a value" >&2
        exit 2
      fi
      SIMPLER_HOST_VECTOR_MANIFEST="$2"
      shift 2
      ;;
    --pto-scenario)
      if [[ $# -lt 2 ]]; then
        echo "--pto-scenario requires a value" >&2
        exit 2
      fi
      SIM_UAPI_SCENARIO_CONFIG="$2"
      shift 2
      ;;
    --pto-artifact-fingerprint)
      if [[ $# -lt 2 ]]; then
        echo "--pto-artifact-fingerprint requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT="$2"
      shift 2
      ;;
    --pto-elements)
      if [[ $# -lt 2 ]]; then
        echo "--pto-elements requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_ELEMENTS="$2"
      shift 2
      ;;
    --pto-layout)
      if [[ $# -lt 2 ]]; then
        echo "--pto-layout requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_LAYOUT="$2"
      shift 2
      ;;
    --pto-generation)
      if [[ $# -lt 2 ]]; then
        echo "--pto-generation requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_GENERATION="$2"
      shift 2
      ;;
    --pto-token-value)
      if [[ $# -lt 2 ]]; then
        echo "--pto-token-value requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_TOKEN_VALUE="$2"
      shift 2
      ;;
    --pto-timeout-ms)
      if [[ $# -lt 2 ]]; then
        echo "--pto-timeout-ms requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_TIMEOUT_MS="$2"
      shift 2
      ;;
    --pto-nodea-cna)
      if [[ $# -lt 2 ]]; then
        echo "--pto-nodea-cna requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_NODEA_CNA="$2"
      shift 2
      ;;
    --pto-nodeb-cna)
      if [[ $# -lt 2 ]]; then
        echo "--pto-nodeb-cna requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_NODEB_CNA="$2"
      shift 2
      ;;
    --pto-authorization-delay-ns)
      if [[ $# -lt 2 ]]; then
        echo "--pto-authorization-delay-ns requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS="$2"
      shift 2
      ;;
    --pto-authorization-timeout-ns)
      if [[ $# -lt 2 ]]; then
        echo "--pto-authorization-timeout-ns requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS="$2"
      shift 2
      ;;
    --pto-cancel-after-ms)
      if [[ $# -lt 2 ]]; then
        echo "--pto-cancel-after-ms requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_CANCEL_AFTER_MS="$2"
      shift 2
      ;;
    --pto-reset-on-pending)
      if [[ $# -lt 2 ]]; then
        echo "--pto-reset-on-pending requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_RESET_ON_PENDING="$2"
      shift 2
      ;;
    --pto-inject-duplicate-completion)
      if [[ $# -lt 2 ]]; then
        echo "--pto-inject-duplicate-completion requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION="$2"
      shift 2
      ;;
    --pto-inject-late-completion)
      if [[ $# -lt 2 ]]; then
        echo "--pto-inject-late-completion requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION="$2"
      shift 2
      ;;
    --pto-expect)
      if [[ $# -lt 2 ]]; then
        echo "--pto-expect requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_EXPECT="$2"
      shift 2
      ;;
    --pto-fault-case)
      if [[ $# -lt 2 ]]; then
        echo "--pto-fault-case requires a value" >&2
        exit 2
      fi
      LINGQU_SHMEM_PTO_FAULT_CASE="$2"
      shift 2
      ;;
    --use-qmp)
      USE_QMP=1
      shift
      ;;
    --use-prebuilt-qemu)
      UB_USE_PREBUILT_QEMU=1
      shift
      ;;
    --interactive-after-pass)
      INTERACTIVE_AFTER_PASS=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$RUN_ID" || "$RUN_ID" == *[^A-Za-z0-9._-]* ]]; then
  echo "run id must contain only ASCII letters, digits, dots, dashes, or underscores" >&2
  exit 2
fi
if [[ "$INTERACTIVE_AFTER_PASS" -eq 1 && "$ITERATIONS" != "1" ]]; then
  echo "interactive-after-pass requires exactly one iteration" >&2
  exit 2
fi

SERIAL_RUNTIME_DIR="/tmp/ubqe_${RUN_ID}"
SERIAL_DIR="$SERIAL_RUNTIME_DIR/serial"
SERIAL_ENV_FILE="$OUT_DIR/dual_node_serial_env.${RUN_ID}.sh"

apply_app_selection "$APP_SELECTION"
if [[ "$APPEND_EXTRA" == *"linqu_shmem_pto_direct=1"* ]]; then
  validate_lingqu_shmem_pto_config
fi
if [[ "$APPEND_EXTRA" == *"linqu_obmm_async_coroutine=1"* ]]; then
  append_cmdline_if_missing "linqu_node_count=2"
  if [[ -z "$OBMM_ASYNC_ARGS" ]]; then
    OBMM_ASYNC_ARGS="--mode async-poll --coroutines 8 --inflight 32 --lookahead 16 --access-bytes 64 --pattern sequential --compute-us 10 --iterations 1024 --deadline-us 1000000 --seed 1 --verify"
  fi
  if [[ "$OBMM_ASYNC_ARGS" == *"--mode async-load"* &&
        -z "$ASYNC_LOAD_MODEL" ]]; then
    echo "async-load mode requires --async-load-model from the scenario" >&2
    exit 2
  fi
  append_obmm_async_args
fi

if [[ -n "$REMOTE_MEMORY_MODEL_MANIFEST" &&
      ! -f "$REMOTE_MEMORY_MODEL_MANIFEST" ]]; then
  echo "remote-memory model manifest does not exist: $REMOTE_MEMORY_MODEL_MANIFEST" >&2
  exit 2
fi
if [[ -n "$REMOTE_MEMORY_MODEL_MANIFEST" ]]; then
  REMOTE_MEMORY_MODEL_MANIFEST="$(cd "$(dirname "$REMOTE_MEMORY_MODEL_MANIFEST")" && pwd)/$(basename "$REMOTE_MEMORY_MODEL_MANIFEST")"
fi

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ "$APPEND_EXTRA" == *"linqu_llm_infer=1"* ]]; then
  append_cmdline_if_missing "pmd_mapping=100%"
  append_cmdline_if_missing "mem_service_region_size_mb=512"
  append_cmdline_if_missing "obmm.mempool_size=512M"
fi

# Reserve 25% of guest RAM for the kernel pfn_range contiguous memory pool.
# OBMM needs large contiguous physical allocations (2MB per segment). Without
# this reservation the buddy allocator fragments over time and OBMM export
# fails with "allocate_memory_contiguous: failed to alloc 0x200000 bytes".
# The pool size is aligned down to PUD_SIZE (1GB on ARM64 4K pages), so 25%
# of 8GB yields 2GB usable.  Must not be set lower than 13% with 8GB guests
# (otherwise ALIGN_DOWN produces 0).
if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

# Keep init alive after probes so the harness can terminate QEMU directly.
# This avoids guest shutdown/remove path stacktraces that are unrelated to
# chat/rpc/udma dataplane validation.
if [[ "$APPEND_EXTRA" != *"linqu_probe_hold="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} linqu_probe_hold=1"
fi

timeout_watchdog() {
  local timeout_sec="$1"
  local sleep_pid=""

  trap 'kill "$sleep_pid" 2>/dev/null || true; wait "$sleep_pid" 2>/dev/null || true; exit 0' TERM INT
  sleep "$timeout_sec" &
  sleep_pid=$!
  wait "$sleep_pid"
  trap - TERM INT
  echo "global timeout ${timeout_sec}s reached, terminating test" >&2
  kill -TERM "$MAIN_PID" 2>/dev/null || true
}

timeout_watchdog "$MAX_RUNTIME" &
WATCHDOG_PID=$!

cleanup_watchdog() {
  if [[ -n "${WATCHDOG_PID:-}" ]]; then
    kill "$WATCHDOG_PID" 2>/dev/null || true
    wait "$WATCHDOG_PID" 2>/dev/null || true
    WATCHDOG_PID=""
  fi
}

cleanup_serial_runtime() {
  rm -rf "$SERIAL_RUNTIME_DIR"
  rm -f "$SERIAL_ENV_FILE"
}

trap 'cleanup_watchdog; cleanup_all_app_pid_files; cleanup_serial_runtime' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

cleanup_pid() {
  local pid_file="$1"
  local pid=""
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file" 2>/dev/null || true)"
  fi
  if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -f "$pid_file"
}

cleanup_all_app_pid_files() {
  local pid_file=""
  for pid_file in "$OUT_DIR"/ub_nodeA.apps.*.pid "$OUT_DIR"/ub_nodeB.apps.*.pid; do
    cleanup_pid "$pid_file"
  done
}

write_serial_manifest() {
  local nodea_socket="$1"
  local nodeb_socket="$2"
  local temporary="${SERIAL_ENV_FILE}.tmp.$$"

  {
    printf "export NODEA_SERIAL_SOCKET='%s'\n" "$nodea_socket"
    printf "export NODEB_SERIAL_SOCKET='%s'\n" "$nodeb_socket"
  } > "$temporary"
  mv "$temporary" "$SERIAL_ENV_FILE"
}

wait_for_log_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]] && grep -qE "$pattern" "$file"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

wait_for_log_pass_or_fail() {
  local file="$1"
  local pass_pattern="$2"
  local fail_pattern="$3"
  local timeout_s="$4"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      if grep -qE "$pass_pattern" "$file"; then
        return 0
      fi
      if grep -qE "$fail_pattern" "$file"; then
        return 1
      fi
    fi
    sleep 0.2
  done
  return 2
}

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! grep -qE "$pattern" "$file"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_absent() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if grep -qE "$pattern" "$file"; then
    echo "unexpected log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_count() {
  local file="$1"
  local pattern="$2"
  local expected="$3"
  local label="$4"
  local actual=""

  actual="$(grep -cE "$pattern" "$file" || true)"
  if [[ "$actual" != "$expected" ]]; then
    echo "unexpected log marker count: $label expected=$expected actual=$actual in $file" >&2
    return 1
  fi
}

validate_chat_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_chat\\] pass" "${node_name} chat pass" || return 1
  assert_log_absent "$log_file" "\\[ub_chat\\] fail" "${node_name} chat fail" || return 1
  assert_log_has "$log_file" "\\[ub_chat\\] summary tx=5 rx=5" "${node_name} chat tx/rx summary" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[CHAT\\] initiator seq=[0-9]+ \"copy, greeting back from responder\"" \
      "${node_name} chat reply payload" || return 1
  else
    assert_log_has "$log_file" "\\[CHAT\\] responder seq=[0-9]+ \"greeting from initiator\"" \
      "${node_name} chat request payload" || return 1
  fi
}

validate_rpc_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_rpc\\] pass" "${node_name} rpc pass" || return 1
  assert_log_absent "$log_file" "\\[ub_rpc\\] fail" "${node_name} rpc fail" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[RPC\\] client local=10\\.0\\.0\\.1 peer=10\\.0\\.0\\.2 op=ECHO msg_id=1 status=OK result=\"greeting from rpc client 10\\.0\\.0\\.1\" expected=\"greeting from rpc client 10\\.0\\.0\\.1\" verified=1" \
      "${node_name} rpc echo semantic" || return 1
    assert_log_has "$log_file" "\\[RPC\\] client local=10\\.0\\.0\\.1 peer=10\\.0\\.0\\.2 op=CRC32 msg_id=2 status=OK payload=\"rpc crc payload from 10\\.0\\.0\\.1 to 10\\.0\\.0\\.2 over ub_link\" result=\"0x[0-9a-f]{8}\" expected=\"0x[0-9a-f]{8}\" verified=1" \
      "${node_name} rpc crc semantic" || return 1
  else
    assert_log_has "$log_file" "\\[RPC\\] server local=10\\.0\\.0\\.2 peer=10\\.0\\.0\\.1 handled op=ECHO msg_id=1 rpc_count=1" \
      "${node_name} rpc server echo handled" || return 1
    assert_log_has "$log_file" "\\[RPC\\] server local=10\\.0\\.0\\.2 peer=10\\.0\\.0\\.1 handled op=CRC32 msg_id=2 rpc_count=2" \
      "${node_name} rpc server crc handled" || return 1
  fi
}

validate_tcp_each_server_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_tcp_each_server\\] pass" \
    "${node_name} tcp each server pass" || return 1
  assert_log_absent "$log_file" "\\[ub_tcp_each_server\\] fail" \
    "${node_name} tcp each server fail" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeA client sent=\"tcp hello from nodeA client\"" \
      "${node_name} tcp client request" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeA server received=\"tcp hello from nodeB client\"" \
      "${node_name} tcp server received peer request" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeA server ack=\"tcp ack from nodeA server\"" \
      "${node_name} tcp server ack" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeA client received_ack=\"tcp ack from nodeB server\"" \
      "${node_name} tcp client ack" || return 1
  else
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeB client sent=\"tcp hello from nodeB client\"" \
      "${node_name} tcp client request" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeB server received=\"tcp hello from nodeA client\"" \
      "${node_name} tcp server received peer request" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeB server ack=\"tcp ack from nodeB server\"" \
      "${node_name} tcp server ack" || return 1
    assert_log_has "$log_file" "\\[TCP_EACH_SERVER\\] nodeB client received_ack=\"tcp ack from nodeA server\"" \
      "${node_name} tcp client ack" || return 1
  fi
}

validate_udma_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_udma\\] pass" "${node_name} udma pass" || return 1
  assert_log_absent "$log_file" "\\[ub_udma\\] fail" "${node_name} udma fail" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 2: alloc_ummu_tid -> ok" \
    "${node_name} udma alloc ummu tid" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 2: alloc_token_id -> ok" \
    "${node_name} udma alloc token id" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 7: register_seg -> ok" \
    "${node_name} udma register seg" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 8: import_jetty -> ok" \
    "${node_name} udma import jetty" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 9: bind_jetty -> ok" \
    "${node_name} udma bind jetty" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: post_recv -> ok" \
    "${node_name} udma post recv" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: ready_sync -> ok" \
    "${node_name} udma ready sync" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: send_request -> ok len=[0-9]+" \
      "${node_name} udma send request" || return 1
    assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: recv_reply -> ok payload=\"udma reply payload from responder\"" \
      "${node_name} udma reply payload" || return 1
  else
    assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: recv_request -> ok payload=\"udma request payload from initiator\"" \
      "${node_name} udma request payload" || return 1
    assert_log_has "$log_file" "\\[ub_udma\\] step 9\\.5: send_reply -> ok len=[0-9]+" \
      "${node_name} udma send reply" || return 1
  fi
  assert_log_has "$log_file" "\\[ub_udma\\] step 10: unbind_jetty -> ok" \
    "${node_name} udma unbind jetty" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] step 10: unimport_jetty -> ok" \
    "${node_name} udma unimport jetty" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] cleanup: unregister_seg -> ok" \
    "${node_name} udma unregister seg cleanup" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] cleanup: free_token_id -> ok" \
    "${node_name} udma free token id cleanup" || return 1
  assert_log_has "$log_file" "\\[ub_udma\\] cleanup: free_ummu_tid -> ok" \
    "${node_name} udma free ummu tid cleanup" || return 1
  assert_log_absent "$log_file" "UDMA: invalid port speed = 0" \
    "${node_name} udma invalid port speed" || return 1
  assert_log_absent "$log_file" "failed to query device status" \
    "${node_name} udma query device status failure" || return 1
  assert_log_absent "$log_file" "ubcore topo map doesn't exist" \
    "${node_name} udma topo map missing" || return 1
  assert_log_absent "$log_file" "UDMA: wait resp timeout" \
    "${node_name} udma wait response timeout" || return 1
  assert_log_absent "$log_file" "fail to notify mue save tp" \
    "${node_name} udma save tp failure" || return 1
  assert_log_absent "$log_file" "ubcore_unimport_jetty_async failed" \
    "${node_name} udma unimport jetty async failure" || return 1
  assert_log_absent "$log_file" "failed to remove uobject" \
    "${node_name} udma uobject cleanup failure" || return 1
  assert_log_absent "$log_file" "invalidate cfg_table failed" \
    "${node_name} udma cfg table cleanup failure" || return 1
}

validate_obmm_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pass" "${node_name} obmm pool pass" || return 1
  assert_log_absent "$log_file" "\\[ub_obmm_pool\\] fail" "${node_name} obmm pool fail" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] export -> ok mem_id=[0-9]+ uba=0x[0-9a-f]+ token=[0-9]+" \
    "${node_name} obmm export" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] metadata exchange -> ok count=2" \
    "${node_name} obmm metadata exchange" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] import_all -> ok remote_slots=1" \
    "${node_name} obmm import all" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool ready -> ok nodes=2" \
    "${node_name} obmm pool ready" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[ub_obmm_pool\\] round owner=1 write_local -> ok slot=1" \
      "${node_name} obmm local write" || return 1
  else
    assert_log_has "$log_file" "\\[ub_obmm_pool\\] round owner=2 write_local -> ok slot=2" \
      "${node_name} obmm local write" || return 1
  fi
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=1 -> ok slot=1" \
    "${node_name} obmm round1 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=2 -> ok slot=2" \
    "${node_name} obmm round2 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool rounds -> ok count=2" \
    "${node_name} obmm rounds done" || return 1
}

validate_obmm_dataplane_microbench_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[obmm_dataplane_microbench\\] result=done" \
    "${node_name} obmm dataplane microbench result" || return 1
  assert_log_absent "$log_file" "bench failed" \
    "${node_name} obmm dataplane microbench failure" || return 1
}

validate_obmm_async_coroutine_log() {
  local node_name="$1"
  local log_file="$2"

  if [[ "$OBMM_ASYNC_ARGS" == *"--mode sync-mmio"* ]]; then
    assert_log_has "$log_file" \
      "OBMM_BASELINE_SUMMARY schema=1 .*status=pass .*failures=0" \
      "${node_name} OBMM sync baseline summary" || return 1
    assert_log_absent "$log_file" \
      "OBMM_BASELINE_SUMMARY .*status=fail" \
      "${node_name} OBMM sync baseline failure" || return 1
    return 0
  fi
  if [[ "$OBMM_ASYNC_ARGS" == *"--mode userfaultfd"* ]]; then
    assert_log_has "$log_file" \
      "OBMM_UFFD_SUMMARY schema=1 .*failures=0 status=pass" \
      "${node_name} OBMM userfaultfd summary" || return 1
    assert_log_absent "$log_file" \
      "OBMM_UFFD_(SUMMARY .*status=fail|FAIL_STOP)" \
      "${node_name} OBMM userfaultfd failure" || return 1
    return 0
  fi
  assert_log_has "$log_file" \
    "OBMM_ASYNC_SUMMARY abi=1 mode=(async-(poll|irq)|async-load) status=pass .*failures=0 .*stale=0" \
    "${node_name} OBMM async summary" || return 1
  assert_log_absent "$log_file" \
    "OBMM_ASYNC_SUMMARY .*status=fail" \
    "${node_name} OBMM async failure" || return 1
}

validate_obmm_import_stress_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[obmm_import_stress\\] result=done" \
    "${node_name} obmm import stress result" || return 1
  assert_log_absent "$log_file" "\\[obmm_import_stress\\] stress_run failed" \
    "${node_name} obmm import stress failure" || return 1
  assert_log_absent "$log_file" "\\[obmm_import_stress\\] verify failure" \
    "${node_name} obmm import stress verify failure" || return 1
}

validate_obmm_gsva_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[obmm_gsva\\] result=done" \
    "${node_name} obmm gsva done" || return 1
  assert_log_absent "$log_file" "\\[obmm_gsva\\] result=fail" \
    "${node_name} obmm gsva failure" || return 1
}

validate_obmm_coh_test_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "obmm_coh_test: FAIL" \
    "${node_name} obmm coh test failure" || return 1
  assert_log_has "$log_file" "obmm_coh_test: PASS" \
    "${node_name} obmm coh test binary pass" || return 1
}

validate_lingqu_shmem_pto_guest_log() {
  local expected_error=""
  local role="$1"
  local log_file="$2"

  case "$LINGQU_SHMEM_PTO_EXPECT" in
    authorization-timeout)
      expected_error="pto_ub_gm_authorization_timeout"
      ;;
    authorization-cancelled)
      expected_error="pto_ub_gm_authorization_cancelled"
      ;;
    bad-memref)
      expected_error="pto_ub_gm_bad_memref"
      ;;
    access-denied)
      expected_error="pto_ub_gm_access_denied"
      ;;
  esac

  assert_log_has "$log_file" \
    "LINGQU_SHMEM_PTO role=$role stage=start .*layout=$LINGQU_SHMEM_PTO_LAYOUT elements=$LINGQU_SHMEM_PTO_ELEMENTS generation=$LINGQU_SHMEM_PTO_GENERATION expected=$LINGQU_SHMEM_PTO_EXPECT fault_case=$LINGQU_SHMEM_PTO_FAULT_CASE" \
    "$role PTO UB_GM start contract" || return 1
  case "$LINGQU_SHMEM_PTO_LAYOUT" in
    nd)
      assert_log_count "$log_file" \
        "LINGQU_SHMEM_PTO role=$role stage=layout layout=nd input_a_offset=0 input_b_offset=65536 output_offset=131072 tensor_bytes=65536 cross_page=1,1,1 offset_mod_64=0,0,0" 1 \
        "$role exact ND layout" || return 1
      ;;
    tail)
      assert_log_count "$log_file" \
        "LINGQU_SHMEM_PTO role=$role stage=layout layout=tail input_a_offset=0 input_b_offset=65024 output_offset=130048 tensor_bytes=65024 cross_page=1,1,1 offset_mod_64=0,0,0" 1 \
        "$role exact tail layout" || return 1
      ;;
    cross-page)
      assert_log_count "$log_file" \
        "LINGQU_SHMEM_PTO role=$role stage=layout layout=cross-page input_a_offset=3968 input_b_offset=8064 output_offset=12160 tensor_bytes=256 cross_page=1,1,1 offset_mod_64=0,0,0" 1 \
        "$role exact cross-page layout" || return 1
      ;;
    unaligned)
      assert_log_count "$log_file" \
        "LINGQU_SHMEM_PTO role=$role stage=layout layout=unaligned input_a_offset=4 input_b_offset=4100 output_offset=8196 tensor_bytes=256 cross_page=0,0,0 offset_mod_64=4,4,4" 1 \
        "$role exact unaligned layout" || return 1
      ;;
  esac
  if [[ "$role" == "producer" ]]; then
    assert_log_has "$log_file" \
      "LINGQU_SHMEM_PTO role=producer stage=published .*elements=$LINGQU_SHMEM_PTO_ELEMENTS generation=$LINGQU_SHMEM_PTO_GENERATION" \
      "producer exported source region" || return 1
    if [[ -n "$expected_error" ]]; then
      assert_log_absent "$log_file" \
        "LINGQU_SHMEM_PTO role=producer producer_verify=pass" \
        "producer write after rejected dispatch" || return 1
      if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "retired-segment" ]]; then
        assert_log_count "$log_file" \
          "LINGQU_SHMEM_PTO role=producer stage=consumer_prepared .*payload_mem_id=[1-9][0-9]*" 1 \
          "producer observed prepared consumer" || return 1
        assert_log_count "$log_file" \
          "LINGQU_SHMEM_PTO role=producer stage=payload_retired payload_mem_id=[1-9][0-9]*" 1 \
          "producer retired payload" || return 1
        assert_log_has "$log_file" \
          "LINGQU_SHMEM_PTO_RESULT role=producer status=pass expected=bad-memref observed=payload_retired output_unchanged=1 elements=$LINGQU_SHMEM_PTO_ELEMENTS sentinel=0x7fc00001" \
          "producer retired unchanged payload" || return 1
      else
        if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "cross-segment" ]]; then
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=producer stage=guard_published .*bytes=2097152 .*generation=[1-9][0-9]* sentinel=0xa5" 1 \
            "producer published adjacent guard segment" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=producer stage=guard_verified guard_unchanged=1 bytes=2097152 sentinel=0xa5" 1 \
            "producer guard segment remained unchanged" || return 1
        fi
        assert_log_has "$log_file" \
          "LINGQU_SHMEM_PTO_RESULT role=producer status=pass expected=$LINGQU_SHMEM_PTO_EXPECT observed=verify_timeout output_unchanged=1 elements=$LINGQU_SHMEM_PTO_ELEMENTS sentinel=0x7fc00001" \
          "producer unchanged output after expected authorization failure" || return 1
      fi
      assert_log_absent "$log_file" \
        "LINGQU_SHMEM_PTO_RESULT role=producer status=fail" \
        "producer unexpected failure result" || return 1
      return 0
    fi
    assert_log_has "$log_file" \
      "LINGQU_SHMEM_PTO role=producer producer_verify=pass elements=$LINGQU_SHMEM_PTO_ELEMENTS" \
      "producer original-mapping verification" || return 1
  else
    assert_log_has "$log_file" \
      "LINGQU_SHMEM_PTO role=consumer stage=prepared .*map_id=[1-9][0-9]* .*map_generation=[1-9][0-9]* .*mapping_ref=0x[1-9a-f][0-9a-f]* .*requester_cna=$LINGQU_SHMEM_PTO_NODEB_CNA .*fingerprint=0x[1-9a-f][0-9a-f]*" \
      "consumer opaque map-ref dispatch" || return 1
    if [[ -n "$expected_error" ]]; then
      case "$LINGQU_SHMEM_PTO_FAULT_CASE" in
        tstore-on-read|tload-on-write)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_selected fault=$LINGQU_SHMEM_PTO_FAULT_CASE source=callable-artifact expected=$LINGQU_SHMEM_PTO_EXPECT " 1 \
            "consumer exact-once requested callable access fault" || return 1
          assert_log_absent "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=$LINGQU_SHMEM_PTO_FAULT_CASE " \
            "consumer wire mutation for callable access fault" || return 1
          ;;
        released-import)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=released-import expected=bad-memref import_mem_id=[1-9][0-9]* import_active=0 map_id=[1-9][0-9]* map_generation=[1-9][0-9]* map_active=1" 1 \
            "consumer exact-once released import fault" || return 1
          assert_log_count "$log_file" \
            "UB SIM Decoder: OBMM unimport unmapped map_id=0x[1-9a-f][0-9a-f]*" 1 \
            "consumer released SIM_DEC import mapping" || return 1
          ;;
        retired-segment)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=prepared_signal .*state=1" 1 \
            "consumer prepared signal" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=retired_observed .*state=2" 1 \
            "consumer observed payload retirement" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=retired-segment expected=bad-memref import_active=1 map_id=[1-9][0-9]* map_generation=[1-9][0-9]* map_active=1" 1 \
            "consumer exact-once retired segment fault" || return 1
          ;;
        shape-stride-oob)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_mutation fault=shape-stride-oob arg_index=2 shape0=16385 stride0=1 extent_bytes=65540 view_bytes=65536" 1 \
            "consumer exact shape-stride extent mutation" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=shape-stride-oob expected=bad-memref " 1 \
            "consumer exact-once shape-stride fault" || return 1
          ;;
        cross-segment)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=guard_import_mapped .*bytes=2097152 .*adjacent=1" 1 \
            "consumer adjacent guard import" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=guard_map_registered .*adjacent_to_map_id=[1-9][0-9]*" 1 \
            "consumer registered second segment mapping" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_mutation fault=cross-segment source_map_id=[1-9][0-9]* source_generation=[1-9][0-9]* guard_map_id=[1-9][0-9]* guard_generation=[1-9][0-9]* guard_mapping_ref=0x[1-9a-f][0-9a-f]* .*adjacent=1" 1 \
            "consumer exact cross-segment range mutation" || return 1
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=cross-segment expected=bad-memref " 1 \
            "consumer exact-once cross-segment fault" || return 1
          ;;
        none)
          ;;
        *)
          assert_log_count "$log_file" \
            "LINGQU_SHMEM_PTO role=consumer stage=fault_injected fault=$LINGQU_SHMEM_PTO_FAULT_CASE expected=$LINGQU_SHMEM_PTO_EXPECT " 1 \
            "consumer exact-once requested wire fault" || return 1
          ;;
      esac
      assert_log_count "$log_file" \
        "LINGQU_SHMEM_PTO role=consumer stage=completion .*completion_status=3 error=$expected_error" 1 \
        "consumer exact-once expected failure completion" || return 1
      assert_log_has "$log_file" \
        "LINGQU_SHMEM_PTO_RESULT role=consumer status=pass expected=$LINGQU_SHMEM_PTO_EXPECT observed=completion error=$expected_error" \
        "consumer expected authorization failure result" || return 1
      assert_log_absent "$log_file" \
        "LINGQU_SHMEM_PTO_RESULT role=consumer status=fail" \
        "consumer unexpected failure result" || return 1
      return 0
    fi
    assert_log_has "$log_file" \
      "LINGQU_SHMEM_PTO role=consumer stage=completion .*completion_status=1 error=none" \
      "consumer successful completion" || return 1
  fi
  assert_log_absent "$log_file" \
    "LINGQU_SHMEM_PTO_RESULT role=$role status=fail" \
    "$role PTO UB_GM failure" || return 1
  assert_log_has "$log_file" \
    "LINGQU_SHMEM_PTO_RESULT role=$role status=pass" \
    "$role PTO UB_GM result" || return 1
}

validate_lingqu_shmem_pto_reset_sequence() {
  local log_file="$1"
  local reset_sequence=""
  local reset_next_sequence=""
  local resumed_sequence=""

  reset_sequence="$(awk '
    /QEMU_UB_GM_RESET authorization_pending=1 / {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^sequence=/) {
          sub(/^sequence=/, "", $i)
          print $i
          exit
        }
      }
    }
  ' "$log_file")"
  reset_next_sequence="$(awk '
    /QEMU_UB_GM_RESET authorization_pending=1 / {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^next_sequence=/) {
          sub(/^next_sequence=/, "", $i)
          print $i
          exit
        }
      }
    }
  ' "$log_file")"
  resumed_sequence="$(awk '
    /QEMU_UB_GM_RESET authorization_pending=1 / {
      reset_seen = 1
      next
    }
    reset_seen && /QEMU_UB_GM_AUTHORIZATION_PENDING / {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^sequence=/) {
          sub(/^sequence=/, "", $i)
          print $i
          exit
        }
      }
    }
  ' "$log_file")"

  if [[ "$reset_sequence" != <-> ||
        "$reset_next_sequence" != <-> ||
        "$resumed_sequence" != <-> ]]; then
    echo "missing PTO reset sequence evidence in $log_file" >&2
    return 1
  fi
  if (( reset_sequence == 0 ||
        reset_next_sequence != reset_sequence ||
        resumed_sequence <= reset_sequence )); then
    echo "invalid PTO reset sequence progression: reset=$reset_sequence next=$reset_next_sequence resumed=$resumed_sequence" >&2
    return 1
  fi
}

validate_lingqu_shmem_pto_layout_accesses() {
  local log_file="$1"
  local layout="$2"
  local tensor_bytes="$3"

  python3 - "$log_file" "$layout" "$tensor_bytes" <<'PY'
import re
import sys

log_path, layout, tensor_bytes_text = sys.argv[1:]
tensor_bytes = int(tensor_bytes_text)
pattern = re.compile(
    r"QEMU_UB_GM_(LOAD|STORE) request=.* addr=0x([0-9a-f]+) "
    r"length=([0-9]+) "
)
accesses = []
with open(log_path, "r", encoding="utf-8", errors="replace") as stream:
    for line in stream:
        match = pattern.search(line)
        if match:
            accesses.append(
                (match.group(1), int(match.group(2), 16), int(match.group(3)))
            )
if len(accesses) != 3:
    raise SystemExit(
        f"expected three PTO UB_GM data callbacks, found {len(accesses)}"
    )
if [kind for kind, _, _ in accesses].count("LOAD") != 2 or \
   [kind for kind, _, _ in accesses].count("STORE") != 1:
    raise SystemExit(f"unexpected PTO UB_GM callback kinds: {accesses!r}")
for kind, address, length in accesses:
    if length != tensor_bytes:
        raise SystemExit(
            f"{kind} length mismatch: expected={tensor_bytes} actual={length}"
        )
    crosses_page = address // 4096 != (address + length - 1) // 4096
    if layout == "cross-page" and not crosses_page:
        raise SystemExit(f"{kind} did not cross a 4 KiB page: addr=0x{address:x}")
    if layout == "unaligned" and (address % 64 != 4 or crosses_page):
        raise SystemExit(
            f"{kind} unaligned contract failed: addr=0x{address:x} "
            f"mod64={address % 64} crosses_page={int(crosses_page)}"
        )
    if layout in ("nd", "tail", "cross-page") and address % 64 != 0:
        raise SystemExit(
            f"{kind} expected 64-byte alignment: addr=0x{address:x}"
        )
PY
}

validate_lingqu_shmem_pto_qemu_log() {
  local node_name="$1"
  local log_file="$2"
  local producer_guest_log="${3:-}"
  local expected_cna="$LINGQU_SHMEM_PTO_NODEA_CNA"
  local tensor_bytes=$((LINGQU_SHMEM_PTO_ELEMENTS * 4))
  local retired_export_cna=""
  local retired_export_mem_id=""

  if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "retired-segment" ]]; then
    if [[ ! -f "$producer_guest_log" ]]; then
      echo "missing producer guest log for retired export identity" >&2
      return 1
    fi
    retired_export_mem_id="$(awk '
      /stage=payload_retired / {
        for (i = 1; i <= NF; i++) {
          if ($i ~ /^payload_mem_id=/) {
            sub(/^payload_mem_id=/, "", $i)
            gsub(/\r/, "", $i)
            print $i
            exit
          }
        }
      }
    ' "$producer_guest_log")"
    if [[ "$retired_export_mem_id" != <-> ||
          "$retired_export_mem_id" == 0 ]]; then
      echo "invalid retired export identity in $producer_guest_log" >&2
      return 1
    fi
    retired_export_cna="$(awk -v expected_mem_id="$retired_export_mem_id" '
      /stage=published / {
        mem_id = ""
        export_cna = ""
        for (i = 1; i <= NF; i++) {
          field = $i
          gsub(/\r/, "", field)
          if (field ~ /^mem_id=/) {
            sub(/^mem_id=/, "", field)
            mem_id = field
          } else if (field ~ /^export_cna=/) {
            sub(/^export_cna=/, "", field)
            export_cna = field
          }
        }
        if (mem_id == expected_mem_id && export_cna != "") {
          print export_cna
          exit
        }
      }
    ' "$producer_guest_log")"
    if ! printf '%s\n' "$retired_export_cna" |
        grep -qE '^0x[0-9a-fA-F]+$'; then
      echo "invalid retired export owner CNA in $producer_guest_log" >&2
      return 1
    fi
  fi

  if [[ "$node_name" == "nodeB" ]]; then
    expected_cna="$LINGQU_SHMEM_PTO_NODEB_CNA"
  fi
  assert_log_absent "$log_file" "QEMU_UB_GM_METADATA_CRC_FAIL" \
    "$node_name PTO metadata CRC failure" || return 1
  assert_log_absent "$log_file" \
    "UB_NPU: created|SIM_DEC: GVA_MAP|GVA_S3_MAP|GVA_ROUTE_DUMP|GSVA_" \
    "$node_name experimental NPU/GVA/GSVA leakage" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(LOAD|STORE|FENCE|UNBIND) " \
      "producer host-side PTO data access" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_(PENDING|RESUME|TIMEOUT|CANCEL|COMPLETION_IGNORED)" \
      "producer PTO authorization activity" || return 1
    if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "retired-segment" ]]; then
      assert_log_count "$log_file" \
        "SIM_DEC: OBMM export retired mem_id=$retired_export_mem_id owner_cna=$retired_export_cna .* generation=$LINGQU_SHMEM_PTO_GENERATION" 1 \
        "producer shared payload retirement tombstone" || return 1
    fi
    return 0
  fi

  assert_log_has "$log_file" \
    "QEMU_UB_GM_ACCESS_REGISTER pto_device_cna=$expected_cna" \
    "$node_name PTO access registration" || return 1
  if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "tstore-on-read" ||
        "$LINGQU_SHMEM_PTO_FAULT_CASE" == "tload-on-write" ]]; then
    local expected_load_bytes=131072

    assert_log_count "$log_file" \
      "QEMU_UB_GM_INPUT_AUTHORIZE " 2 \
      "consumer execution-fault input authorization" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_OUTPUT_AUTHORIZE " 1 \
      "consumer execution-fault output authorization" || return 1
    assert_log_count "$log_file" \
      "SIM_QEMU_UB_GM_BIND_REGISTER .*bindings=3 requester_cna=$expected_cna" 1 \
      "consumer execution-fault binding registration" || return 1
    assert_log_count "$log_file" \
      "linqu-uapi flush_cq wrote completion .*status=3 .*code=pto_ub_gm_access_denied" 1 \
      "consumer execution-fault CQ completion" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_UNBIND .*reason=completion_failure bindings=3 load_bytes=$expected_load_bytes store_bytes=0 fences=0 segment_payload_staging_bytes=0" 1 \
      "consumer execution-fault cleanup" || return 1
    assert_log_count "$log_file" "QEMU_UB_GM_LOAD " 2 \
      "consumer valid loads before denied PTO access" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(STORE|FENCE|FAILURE_COMPLETION|DISPATCH_REJECT)" \
      "consumer backend activity after PTO access denial" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_(PENDING|RESUME|TIMEOUT|CANCEL|COMPLETION_IGNORED)" \
      "consumer asynchronous authorization during execution access fault" || return 1
    return 0
  fi
  if [[ "$LINGQU_SHMEM_PTO_EXPECT" == "bad-memref" ||
        "$LINGQU_SHMEM_PTO_EXPECT" == "access-denied" ]]; then
    local expected_error="pto_ub_gm_bad_memref"

    if [[ "$LINGQU_SHMEM_PTO_EXPECT" == "access-denied" ]]; then
      expected_error="pto_ub_gm_access_denied"
    fi
    assert_log_count "$log_file" \
      "QEMU_UB_GM_DISPATCH_REJECT .*error=[0-9]+ code=$expected_error" 1 \
      "consumer exact-once wire-fault rejection" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_FAILURE_COMPLETION .*cq_slot=0 cq_tail=1 status=3 code=$expected_error" 1 \
      "consumer exact-once wire-fault CQ completion" || return 1
    assert_log_count "$log_file" \
      "linqu-uapi kick ring queued=0 consumed=1 pending_head=1 tail=1" 1 \
      "consumer immediate wire-fault CMDQ retirement" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_(PENDING|RESUME|TIMEOUT|CANCEL|COMPLETION_IGNORED)" \
      "consumer authorization activity after synchronous wire fault" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(INPUT_AUTHORIZE|OUTPUT_AUTHORIZE|INOUT_AUTHORIZE|LOAD|STORE|FENCE|UNBIND)|SIM_QEMU_UB_GM_BIND_REGISTER" \
      "consumer data access after wire fault" || return 1
    if [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "released-import" ]]; then
      assert_log_count "$log_file" \
        "SIM_DEC: UNMAP success id=[1-9a-f][0-9a-f]*" 1 \
        "consumer SIM_DEC import unmap" || return 1
    elif [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "retired-segment" ]]; then
      assert_log_count "$log_file" \
        "SIM_DEC: MAP success .* generation=$LINGQU_SHMEM_PTO_GENERATION export_mem_id=$retired_export_mem_id" 1 \
        "consumer mapped exact payload export lifetime" || return 1
      assert_log_count "$log_file" \
        "SIM_DEC: OBMM remote export retired map_id=[1-9][0-9]* owner_cna=$retired_export_cna .* generation=$LINGQU_SHMEM_PTO_GENERATION export_mem_id=$retired_export_mem_id" 1 \
        "consumer observed shared payload retirement tombstone" || return 1
    elif [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "shape-stride-oob" ]]; then
      assert_log_count "$log_file" \
        "QEMU_UB_GM_SHAPE_STRIDE_REJECT .*arg=2 rank=1 extent_bytes=65540 view_bytes=65536" 1 \
        "consumer QEMU shape-stride extent rejection" || return 1
      assert_log_absent "$log_file" \
        "QEMU_UB_GM_MAPPING_BOUNDARY_REJECT " \
        "consumer mapping-boundary rejection during shape fault" || return 1
    elif [[ "$LINGQU_SHMEM_PTO_FAULT_CASE" == "cross-segment" ]]; then
      assert_log_count "$log_file" \
        "QEMU_UB_GM_MAPPING_BOUNDARY_REJECT .*arg=2 source_map=[1-9][0-9]* source_generation=[1-9][0-9]* .*source_length=2097152 .*adjacent_map=[1-9][0-9]* adjacent_generation=[1-9][0-9]* .*request_length=65536" 1 \
        "consumer QEMU cross-segment mapping rejection" || return 1
      assert_log_absent "$log_file" \
        "QEMU_UB_GM_SHAPE_STRIDE_REJECT " \
        "consumer shape rejection during cross-segment fault" || return 1
    fi
    return 0
  fi
  if [[ "$LINGQU_SHMEM_PTO_EXPECT" == "authorization-timeout" ]]; then
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_PENDING .*cursor=0 .*sequence=[1-9][0-9]* .*delay_ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS " 1 \
      "consumer first-range pending authorization" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_RESUME .*cursor=0 .*sequence=[1-9][0-9]* status=timeout" 1 \
      "consumer timeout resume" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_TIMEOUT .*cursor=0 .*sequence=[1-9][0-9]*" 1 \
      "consumer authorization timeout" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_DISPATCH_REJECT .*code=pto_ub_gm_authorization_timeout" 1 \
      "consumer exact-once rejected dispatch" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_FAILURE_COMPLETION .*cq_slot=0 cq_tail=1 status=3 code=pto_ub_gm_authorization_timeout" 1 \
      "consumer exact-once timeout CQ completion" || return 1
    assert_log_count "$log_file" \
      "linqu-uapi kick ring queued=0 consumed=0 pending_head=0 tail=1" 1 \
      "consumer retained CMDQ head while pending" || return 1
    assert_log_count "$log_file" \
      "linqu-uapi kick ring queued=0 consumed=1 pending_head=1 tail=1" 1 \
      "consumer advanced CMDQ head after timeout completion" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(INPUT_AUTHORIZE|OUTPUT_AUTHORIZE|INOUT_AUTHORIZE|LOAD|STORE|FENCE|UNBIND)|SIM_QEMU_UB_GM_BIND_REGISTER" \
      "consumer data access after authorization timeout" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_(CANCEL|COMPLETION_IGNORED)" \
      "consumer cancellation activity during authorization timeout" || return 1
    return 0
  fi
  if [[ "$LINGQU_SHMEM_PTO_EXPECT" == "authorization-cancelled" ]]; then
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_PENDING .*cursor=0 .*sequence=[1-9][0-9]* .*delay_ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS " 1 \
      "consumer first-range pending authorization before cancel" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_FAILURE_COMPLETION .*cq_slot=0 cq_tail=1 status=3 code=pto_ub_gm_authorization_cancelled" 1 \
      "consumer exact-once cancellation CQ completion" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_DISPATCH_REJECT .*code=pto_ub_gm_authorization_cancelled" 1 \
      "consumer exact-once cancelled dispatch" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_CANCEL .*slot=0 .*sequence=[1-9][0-9]* cmdq_head=1 cq_tail=1" 1 \
      "consumer cancel advanced matching CMDQ head" || return 1
    assert_log_count "$log_file" \
      "linqu-uapi kick ring queued=0 consumed=0 pending_head=0 tail=1" 1 \
      "consumer retained CMDQ head before cancel" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_CANCEL_IGNORED" \
      "consumer ignored cancellation request" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_(RESUME|TIMEOUT)" \
      "consumer timer completion after cancel" || return 1
    if (( LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION != 0 )); then
      assert_log_count "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED .*source=cancel-late-injection reason=no_pending" 1 \
        "consumer ignored injected late completion" || return 1
    else
      assert_log_absent "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED" \
        "consumer unexpected late completion" || return 1
    fi
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(INPUT_AUTHORIZE|OUTPUT_AUTHORIZE|INOUT_AUTHORIZE|LOAD|STORE|FENCE|UNBIND)|SIM_QEMU_UB_GM_BIND_REGISTER" \
      "consumer data access after authorization cancellation" || return 1
    return 0
  fi

  if (( LINGQU_SHMEM_PTO_RESET_ON_PENDING != 0 )); then
    assert_log_count "$log_file" \
      "QEMU_UB_GM_RESET authorization_pending=1 .*sequence=[1-9][0-9]* cq_completion=0 next_sequence=[1-9][0-9]* sim_dec_unmaps=1 obmm_async_reset=1" 1 \
      "consumer exact-once pending reset" || return 1
    assert_log_count "$log_file" \
      "SIM_DEC: RESET_UNMAP count=1 next_map_id=[2-9][0-9]*" 1 \
      "consumer reset import-map retirement" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_PENDING .*cursor=[0-2] .*delay_ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS " 4 \
      "consumer pre-reset and rebooted authorization ranges" || return 1
    assert_log_count "$log_file" \
      "QEMU_UB_GM_AUTHORIZATION_RESUME .*cursor=[0-2] .*status=ready" 3 \
      "consumer rebooted authorization resumes" || return 1
    assert_log_absent "$log_file" \
      "QEMU_UB_GM_(FAILURE_COMPLETION|DISPATCH_REJECT|AUTHORIZATION_TIMEOUT|AUTHORIZATION_CANCEL)" \
      "consumer reset failure completion" || return 1
    if (( LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION != 0 )); then
      assert_log_count "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED .*source=reset-late-injection reason=no_pending" 1 \
        "consumer ignored post-reset late completion" || return 1
    else
      assert_log_absent "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED" \
        "consumer unexpected post-reset completion" || return 1
    fi
    validate_lingqu_shmem_pto_reset_sequence "$log_file" || return 1
  else
    assert_log_absent "$log_file" "QEMU_UB_GM_DISPATCH_REJECT" \
      "consumer PTO dispatch rejection" || return 1
    assert_log_absent "$log_file" "QEMU_UB_GM_AUTHORIZATION_TIMEOUT" \
      "consumer PTO authorization timeout" || return 1
    assert_log_absent "$log_file" "QEMU_UB_GM_AUTHORIZATION_CANCEL" \
      "consumer unexpected PTO authorization cancel" || return 1
    if (( LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS == 0 )); then
      assert_log_absent "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_(PENDING|RESUME)" \
        "synchronous PTO authorization delay" || return 1
    else
      assert_log_count "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_PENDING .*cursor=[0-2] .*delay_ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS " 3 \
        "consumer pending authorization ranges" || return 1
      assert_log_count "$log_file" \
        "QEMU_UB_GM_AUTHORIZATION_RESUME .*cursor=[0-2] .*status=ready" 3 \
        "consumer resumed authorization ranges" || return 1
      if (( LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION != 0 )); then
        assert_log_count "$log_file" \
          "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED .*source=duplicate-injection reason=already_completed" 3 \
          "consumer ignored duplicate authorization completions" || return 1
      else
        assert_log_absent "$log_file" \
          "QEMU_UB_GM_AUTHORIZATION_COMPLETION_IGNORED" \
          "consumer unexpected duplicate authorization completion" || return 1
      fi
    fi
  fi

  assert_log_count "$log_file" "QEMU_UB_GM_INPUT_AUTHORIZE " 2 \
    "consumer input authorizations" || return 1
  assert_log_count "$log_file" "QEMU_UB_GM_OUTPUT_AUTHORIZE " 1 \
    "consumer output authorization" || return 1
  assert_log_has "$log_file" \
    "SIM_QEMU_UB_GM_BIND_REGISTER .*bindings=3 requester_cna=$LINGQU_SHMEM_PTO_NODEB_CNA" \
    "consumer three-memref binding" || return 1
  assert_log_count "$log_file" "QEMU_UB_GM_LOAD request=.*length=$tensor_bytes " 2 \
    "consumer PTO TLOAD callbacks" || return 1
  assert_log_count "$log_file" "QEMU_UB_GM_STORE request=.*length=$tensor_bytes " 1 \
    "consumer PTO TSTORE callback" || return 1
  assert_log_count "$log_file" "QEMU_UB_GM_FENCE request=.*length=$tensor_bytes" 1 \
    "consumer PTO write fence" || return 1
  validate_lingqu_shmem_pto_layout_accesses \
    "$log_file" "$LINGQU_SHMEM_PTO_LAYOUT" "$tensor_bytes" || return 1
  assert_log_has "$log_file" \
    "QEMU_UB_GM_UNBIND .*reason=completion_success bindings=3 load_bytes=$((tensor_bytes * 2)) store_bytes=$tensor_bytes fences=1 segment_payload_staging_bytes=0" \
    "consumer zero-staging completion unbind" || return 1
}

validate_mem_service_obmm_provider_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" \
    "mem_service obmm-provider-conformance: status=fail" \
    "${node_name} OBMM provider conformance failure" || return 1
  assert_log_has "$log_file" \
    "stage=pre-canary readiness=degraded data_plane_ready=0" \
    "${node_name} OBMM provider fail-closed pre-canary readiness" || return 1
  assert_log_has "$log_file" \
    "stage=post-canary readiness=ready data_plane_ready=1" \
    "${node_name} OBMM provider verified readiness" || return 1
  assert_log_has "$log_file" \
    "mem_service obmm-provider-conformance: status=ok node=[01].*mapping=sim-dec.*visibility=checksum-verified.*cleanup=verified" \
    "${node_name} OBMM provider neutral contract conformance" || return 1
}

validate_mem_service_obmm_provider_qemu_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "GSVA_MAP: obmm bootstrap route" \
    "${node_name} OBMM provider GSVA route evidence" || return 1
  assert_log_has "$log_file" "SIM_DEC: MAP success" \
    "${node_name} OBMM provider remote import evidence" || return 1
}

validate_gva_direct_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "\\[gva_direct\\] result=fail" \
    "${node_name} gva_direct failure" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[gva_direct\\] result=done mode=${GVA_DIRECT_MODE} role=home" \
      "${node_name} gva_direct home result" || return 1
  else
    assert_log_has "$log_file" "\\[gva_direct\\] result=done mode=${GVA_DIRECT_MODE} role=peer" \
      "${node_name} gva_direct peer result" || return 1
  fi
}

validate_gsva_query_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "verdict=FAIL" \
    "${node_name} gsva query failure" || return 1
  assert_log_has "$log_file" "\\[gsva_query\\] GSVA_QUERY_" \
    "${node_name} gsva query result" || return 1
  assert_log_has "$log_file" "verdict=PASS" \
    "${node_name} gsva query verdict" || return 1
}

validate_npu_test_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "\\[npu_test\\] verdict=FAIL" \
    "${node_name} npu test failure" || return 1
  assert_log_has "$log_file" "\\[npu_test\\] NPU test suite" \
    "${node_name} npu test suite started" || return 1
  assert_log_has "$log_file" "\\[npu_test\\] verdict=(PASS|SKIP)" \
    "${node_name} npu test verdict" || return 1
}

validate_ssd_test_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "\\[ssd_test\\] verdict=FAIL" \
    "${node_name} ssd test failure" || return 1
  assert_log_has "$log_file" "\\[ssd_test\\] SSD test suite" \
    "${node_name} ssd test suite started" || return 1
  assert_log_has "$log_file" "\\[ssd_test\\] verdict=PASS" \
    "${node_name} ssd test verdict" || return 1
}

validate_ssd_gsva_test_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "\\[ssd_gsva_test\\]verdict=FAIL" \
    "${node_name} ssd gsva test failure" || return 1
  assert_log_has "$log_file" "\\[ssd_gsva_test\\]SSD GSVA data test suite" \
    "${node_name} ssd gsva test suite started" || return 1
  assert_log_has "$log_file" "\\[ssd_gsva_test\\]verdict=PASS" \
    "${node_name} ssd gsva test verdict" || return 1
}

validate_w4_guest_log() {
  local node_name="$1"
  local log_file="$2"

  assert_log_absent "$log_file" "\\[w4_guest\\] fail" \
    "${node_name} w4 guest failure" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_kvcache_path=ready" \
    "${node_name} w4 obmm kvcache backing" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_cluster_mode=resource_backed_uapi" \
    "${node_name} w4 resource-backed db cluster" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] assessment service_coverage=5/5 .* complete=true" \
    "${node_name} w4 service coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] dispatch path=ubc_entity_chipbackend" \
    "${node_name} w4 chipbackend dispatch" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] pass" \
    "${node_name} w4 pass" || return 1
}

validate_mem_service_log() {
  local node_name="$1"
  local log_file="$2"

  assert_log_absent "$log_file" "mem_service smoke: .* failed" \
    "${node_name} mem_service smoke failure" || return 1
  assert_log_has "$log_file" "mem_service smoke: status=ok records=[0-9]+ block_key=block/cli-block-hash state=reloaded group_members=2" \
    "${node_name} mem_service smoke pass" || return 1
}

validate_kernel_health_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "WARNING: CPU:" "${node_name} kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "${node_name} stacktrace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "${node_name} panic" || return 1
}

ipourma_ipv4_args_for_role() {
  local role="$1"
  case "$role" in
    nodeA)
      echo "linqu_ipourma_ipv4=10.0.0.1 linqu_ipourma_peer_ipv4=10.0.0.2"
      ;;
    nodeB)
      echo "linqu_ipourma_ipv4=10.0.0.2 linqu_ipourma_peer_ipv4=10.0.0.1"
      ;;
    *)
      ;;
  esac
}

start_node() {
  local node_id="$1"
  local role="$2"
  local guest_log="$3"
  local qemu_log="$4"
  local pid_file="$5"
  local qmp_socket="$6"
  local serial_socket="$7"
  local app_append_extra="${8-}"
  local qemu_extra=()
  local experimental_features="${UB_SIM_EXPERIMENTAL_FEATURES:-}"
  local qemu_control_args=()
  local remote_model_args=()
  local async_load_args=()
  local pto_ub_gm_args=()
  local pto_duplicate_completion="off"
  local pto_late_completion="off"
  local serial_args=()
  local node_append_extra="$APPEND_EXTRA"
  local ipourma_args=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  ipourma_args="$(ipourma_ipv4_args_for_role "$role")"
  if [[ -n "$ipourma_args" ]]; then
    node_append_extra="${node_append_extra} ${ipourma_args}"
  fi
  if [[ -n "${app_append_extra}" ]]; then
    node_append_extra="${node_append_extra} ${app_append_extra}"
  fi

  if [[ "$USE_QMP" == "1" ]]; then
    mkdir -p "$(dirname "$qmp_socket")"
    qemu_control_args=(-S -qmp unix:"$qmp_socket",server=on,wait=off)
  fi
  if [[ -n "$REMOTE_MEMORY_MODEL_MANIFEST" ]]; then
    remote_model_args=(
      -global "ubc.remote-memory-model-manifest=$REMOTE_MEMORY_MODEL_MANIFEST"
    )
  fi
  if [[ -n "$ASYNC_LOAD_MODEL" ]]; then
    async_load_args=(
      -global "ubc.async-load-model=$ASYNC_LOAD_MODEL"
    )
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_shmem_pto_direct=1"* ]]; then
    if (( LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION != 0 )); then
      pto_duplicate_completion="on"
    fi
    if (( LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION != 0 )); then
      pto_late_completion="on"
    fi
    case "$role" in
      nodeA)
        pto_ub_gm_args=(
          -global "ubc.pto-device-cna=$LINGQU_SHMEM_PTO_NODEA_CNA"
          -global "ubc.pto-authorization-delay-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS"
          -global "ubc.pto-authorization-timeout-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS"
          -global "ubc.pto-authorization-inject-duplicate-completion=$pto_duplicate_completion"
          -global "ubc.pto-authorization-inject-late-completion=$pto_late_completion"
        )
        ;;
      nodeB)
        pto_ub_gm_args=(
          -global "ubc.pto-device-cna=$LINGQU_SHMEM_PTO_NODEB_CNA"
          -global "ubc.pto-authorization-delay-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS"
          -global "ubc.pto-authorization-timeout-ns=$LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS"
          -global "ubc.pto-authorization-inject-duplicate-completion=$pto_duplicate_completion"
          -global "ubc.pto-authorization-inject-late-completion=$pto_late_completion"
        )
        ;;
      *)
        echo "unknown PTO UB_GM node role: $role" >&2
        return 2
        ;;
    esac
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_npu_test=1"* ]]; then
    if [[ ",${experimental_features}," != *",npu,"* ]]; then
      experimental_features="${experimental_features:+${experimental_features},}npu"
    fi
  elif [[ "$APPEND_EXTRA" == *"linqu_obmm_gsva=1"* ||
          "$APPEND_EXTRA" == *"linqu_gva_direct=1"* ||
          "$APPEND_EXTRA" == *"linqu_gsva_query=1"* ||
          "$APPEND_EXTRA" == *"linqu_ssd_gsva_test=1"* ]]; then
    if [[ ",${experimental_features}," != *",gsva,"* ]]; then
      experimental_features="${experimental_features:+${experimental_features},}gsva"
    fi
  fi
  mkdir -p "$(dirname "$guest_log")"
  mkdir -p "$(dirname "$qemu_log")"
  if [[ "$INTERACTIVE_AFTER_PASS" -eq 1 ]]; then
    mkdir -p "$(dirname "$serial_socket")"
    rm -f "$serial_socket"
    serial_args=(
      -chardev "socket,id=ser0,path=$serial_socket,server=on,wait=off,logfile=$guest_log,logappend=off"
      -serial chardev:ser0
    )
  else
    serial_args=(-serial "file:$guest_log")
  fi

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_SIM_EXPERIMENTAL_FEATURES="$experimental_features" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    SIMPLER_HOST_VECTOR_MANIFEST="$SIMPLER_HOST_VECTOR_MANIFEST" \
    SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
    SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
    SIM_UAPI_SCENARIO_CONFIG="$SIM_UAPI_SCENARIO_CONFIG" \
    "$QEMU_BIN" \
      "${qemu_control_args[@]}" \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -m 8G \
      -nodefaults \
      -nographic \
      "${remote_model_args[@]}" \
      "${async_load_args[@]}" \
      "${pto_ub_gm_args[@]}" \
      "${serial_args[@]}" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=${RDINIT} linqu_urma_dp_role=${role} ${node_append_extra}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

wait_for_fm_links_ready() {
  local nodea_log="$1"
  local nodeb_log="$2"
  local timeout_s="${3:-30}"
  local deadline=$((SECONDS + timeout_s))
  local nodea_status="$SHARED_DIR/nodeA_ubcdev0__1.status"
  local nodeb_status="$SHARED_DIR/nodeB_ubcdev0__1.status"

  echo "Waiting for FM links to be ready (timeout ${timeout_s}s)..."

  while (( SECONDS < deadline )); do
    local nodea_ready=false
    local nodeb_ready=false

    if [[ -f "$nodea_status" ]]; then
      local state=$(grep "^state=" "$nodea_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then
        nodea_ready=true
      fi
    fi
    if [[ "$nodea_ready" == "false" ]] && [[ -f "$nodea_log" ]] && \
       grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodea_log"; then
      nodea_ready=true
    fi

    if [[ -f "$nodeb_status" ]]; then
      local state=$(grep "^state=" "$nodeb_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then
        nodeb_ready=true
      fi
    fi
    if [[ "$nodeb_ready" == "false" ]] && [[ -f "$nodeb_log" ]] && \
       grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodeb_log"; then
      nodeb_ready=true
    fi

    if [[ "$nodea_ready" == "true" && "$nodeb_ready" == "true" ]]; then
      echo "FM links ready!"
      return 0
    fi

    sleep 0.2
  done

  echo "FM links NOT ready within timeout!" >&2
  echo "=== nodeA link status ===" >&2
  if [[ -f "$nodea_status" ]]; then
    cat "$nodea_status" >&2
  else
    echo "(no status file: $nodea_status)" >&2
  fi
  echo "=== nodeB link status ===" >&2
  if [[ -f "$nodeb_status" ]]; then
    cat "$nodeb_status" >&2
  else
    echo "(no status file: $nodeb_status)" >&2
  fi
  return 1
}

check_entity_ready() {
  local node="$1"
  local log_file="$2"
  local timeout_sec="${3:-30}"
  local expected_count="${4:-2}"

  echo "Checking entity readiness on ${node} (timeout ${timeout_sec}s, expected ${expected_count} entities)..."

  local elapsed=0
  while [ $elapsed -lt $timeout_sec ]; do
    if [[ -f "$log_file" ]]; then
      local count=$(grep -cE "entity_reg inject SUCCESS|entity_table_init:.*state=present|entity_plan: loaded entity .* state=present" "$log_file" 2>/dev/null || echo "0")
      if [ "$count" -ge "$expected_count" ]; then
        echo "PASS: Entities ready on ${node} (${count} entities)"
        return 0
      fi
    fi
    sleep 0.5
    elapsed=$((elapsed + 1))
  done

  echo "FAIL: Entities not ready on ${node} after ${timeout_sec}s"
  return 1
}

dump_link_diagnostics() {
  local nodea_log="$1"
  local nodeb_log="$2"
  local nodea_status="$SHARED_DIR/nodeA_ubcdev0__1.status"
  local nodeb_status="$SHARED_DIR/nodeB_ubcdev0__1.status"

  echo "=== Link Diagnostics ===" >&2
  printf "%-10s %-10s %-15s %-15s %-10s\n" "Node" "Socket" "RemoteGUID" "State" "Error" >&2
  printf "%-10s %-10s %-15s %-15s %-10s\n" "----" "------" "-----------" "-----" "-----" >&2

  for node in "nodeA" "nodeB"; do
    local status_file="${SHARED_DIR}/${node}_ubcdev0__1.status"
    if [[ -f "$status_file" ]]; then
      local socket=$(grep "^socket_connected=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local guid=$(grep "^remote_guid_valid=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local state=$(grep "^state=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local error=$(grep "^last_error=" "$status_file" 2>/dev/null | cut -d'=' -f2 | sed 's/"//g')

      socket=${socket:-false}
      guid=${guid:-false}
      state=${state:-UNKNOWN}
      error=${error:-none}

      printf "%-10s %-10s %-15s %-15s %-10s\n" \
        "$node" "$socket" "$guid" "$state" "$error" >&2
    else
      printf "%-10s %-10s %-15s %-15s %-10s\n" \
        "$node" "N/A" "N/A" "NO_STATUS" "missing_file" >&2
    fi
  done

  echo "=== Recent log markers ===" >&2
  echo "nodeA:" >&2
  grep -nE "ub_link:|ub_fm:" "$nodea_log" 2>/dev/null | tail -10 >&2 || true
  echo "nodeB:" >&2
  grep -nE "ub_link:|ub_fm:" "$nodeb_log" 2>/dev/null | tail -10 >&2 || true
}

qmp_execute_strict() {
  local qmp_socket="$1"
  local command="$2"
  local node_name="$3"
  local timeout_s="${4:-10}"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    if [[ -S "$qmp_socket" ]]; then
      break
    fi
    sleep 0.1
  done
  if [[ ! -S "$qmp_socket" ]]; then
    echo "QMP socket not ready for $node_name: $qmp_socket" >&2
    return 1
  fi

  if ! python3 - "$qmp_socket" "$command" <<'PY'
import json
import socket
import sys
import time


path, command = sys.argv[1:]
sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
sock.settimeout(5)
sock.connect(path)
buffer = b""


def receive():
    global buffer
    while b"\n" not in buffer:
        chunk = sock.recv(4096)
        if not chunk:
            raise RuntimeError("QMP socket closed")
        buffer += chunk
    line, buffer = buffer.split(b"\n", 1)
    return json.loads(line.strip())


def wait_for(identifier):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        message = receive()
        if message.get("id") == identifier:
            if "error" in message:
                raise RuntimeError(json.dumps(message["error"], sort_keys=True))
            return
    raise RuntimeError(f"timed out waiting for QMP response {identifier}")


try:
    greeting = receive()
    if "QMP" not in greeting:
        raise RuntimeError("missing QMP greeting")
    sock.sendall(
        json.dumps({"execute": "qmp_capabilities", "id": "capabilities"})
        .encode("ascii")
        + b"\r\n"
    )
    wait_for("capabilities")
    sock.sendall(
        json.dumps({"execute": command, "id": "command"}).encode("ascii")
        + b"\r\n"
    )
    wait_for("command")
finally:
    sock.close()
PY
  then
    echo "QMP command failed for $node_name: $command" >&2
    return 1
  fi
  echo "QMP command completed for $node_name: $command"
}

cont_qemu() {
  local qmp_socket="$1"
  local node_name="$2"
  local timeout_s="${3:-10}"

  echo "Resuming $node_name via QMP ($qmp_socket)..."

  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -S "$qmp_socket" ]]; then
      break
    fi
    sleep 0.1
  done

  if [[ ! -S "$qmp_socket" ]]; then
    echo "QMP socket not ready: $qmp_socket" >&2
    return 0
  fi

  python3 -c "
import socket
import json
import sys
import traceback
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect('${qmp_socket}')
    s.recv(1024)
    s.sendall(b'{\\\"execute\\\": \\\"qmp_capabilities\\\"}\r\n')
    s.recv(1024)
    s.sendall(b'{\\\"execute\\\": \\\"cont\\\"}\r\n')
    resp = s.recv(1024)
    s.close()
    if b'return' in resp or b'error' in resp or b'CommandNotFound' in resp:
        sys.exit(0)
    else:
        sys.exit(0)
except Exception as e:
    sys.exit(0)
" 2>/dev/null || true

  if [[ $? -eq 0 ]]; then
    echo "$node_name resumed"
  else
    echo "QMP resume had issues, but continuing anyway" >&2
  fi
  return 0
}

check_link_early_or_fail() {
  local nodea_log="$1"
  local nodeb_log="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  local fail_pat="ub_link: server listen failed|ub_link: failed to connect remote server|bizmsg roundtrip fail: remote linkup not ready|\\[init\\] ub sysfs wait timed out|Failed to bind socket|Failed to bind|failed to create listener|Address already in use"
  local ok_pat="ub_link: connected to remote server|ub_link: accepted connection|remote snapshot load done|remote cfg notify done"

  while (( SECONDS < deadline )); do
    if [[ -f "$nodea_log" ]] && grep -qE "$fail_pat" "$nodea_log"; then
      echo "early qemu/link failure detected on nodeA" >&2
      dump_link_diagnostics "$nodea_log" "$nodeb_log"
      return 1
    fi
    if [[ -f "$nodeb_log" ]] && grep -qE "$fail_pat" "$nodeb_log"; then
      echo "early qemu/link failure detected on nodeB" >&2
      dump_link_diagnostics "$nodea_log" "$nodeb_log"
      return 1
    fi

    if [[ -f "$nodea_log" && -f "$nodeb_log" ]] &&
       grep -qE "$ok_pat" "$nodea_log" &&
       grep -qE "$ok_pat" "$nodeb_log"; then
      return 0
    fi

    sleep 0.2
  done

  return 0
}

run_iteration() {
  local iter="$1"
  local iter_log_dir="$LOG_DIR/${RUN_ID}_apps_iter${iter}"
  local nodea_guest_log="$iter_log_dir/nodeA_guest.log"
  local nodeb_guest_log="$iter_log_dir/nodeB_guest.log"
  local nodea_qemu_log="$iter_log_dir/nodeA_qemu.log"
  local nodeb_qemu_log="$iter_log_dir/nodeB_qemu.log"
  local nodea_log_link="$OUT_DIR/ub_nodeA.apps.${iter}.log"
  local nodeb_log_link="$OUT_DIR/ub_nodeB.apps.${iter}.log"
  local nodea_qemu_log_link="$OUT_DIR/ub_nodeA.apps.${iter}.qemu.log"
  local nodeb_qemu_log_link="$OUT_DIR/ub_nodeB.apps.${iter}.qemu.log"
  local nodea_pid_file="$OUT_DIR/ub_nodeA.apps.${iter}.pid"
  local nodeb_pid_file="$OUT_DIR/ub_nodeB.apps.${iter}.pid"
  local nodea_qmp="$QMP_DIR/nodeA.${iter}.sock"
  local nodeb_qmp="$QMP_DIR/nodeB.${iter}.sock"
  local nodea_serial="$SERIAL_DIR/nodeA.${iter}.sock"
  local nodeb_serial="$SERIAL_DIR/nodeB.${iter}.sock"
  local chat_enabled=0
  local rpc_enabled=0
  local tcp_enabled=0
  local udma_enabled=0
  local obmm_enabled=0
  local obmm_dataplane_microbench_enabled=0
  local obmm_async_coroutine_enabled=0
  local obmm_import_stress_enabled=0
  local obmm_gsva_enabled=0
  local obmm_coh_test_enabled=0
  local mem_service_obmm_provider_enabled=0
  local gva_direct_enabled=0
  local gsva_query_enabled=0
  local npu_test_enabled=0
  local ssd_test_enabled=0
  local ssd_gsva_test_enabled=0
  local mem_service_enabled=0
  local w4_guest_enabled=0
  local pretraining_client_mem_service_enabled=0
  local lingqu_shmem_pto_enabled=0
  local nodea_obmm_coh_test_append=""
  local nodeb_obmm_coh_test_append=""
  local nodea_mem_service_obmm_provider_append=""
  local nodeb_mem_service_obmm_provider_append=""
  local nodea_ssd_gsva_test_append=""
  local nodeb_ssd_gsva_test_append=""
  local nodea_w4_guest_append=""
  local nodeb_w4_guest_append=""
  local nodea_lingqu_shmem_pto_append=""
  local nodeb_lingqu_shmem_pto_append=""
  local nodea_app_append=""
  local nodeb_app_append=""
  local wait_status=0
  local stale_files=()

  if [[ "$APPEND_EXTRA" == *"linqu_ub_chat=1"* ]]; then
    chat_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_rpc=1"* ]]; then
    rpc_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_tcp_each_server=1"* ]]; then
    tcp_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_udma=1"* ]]; then
    udma_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_pool=1"* ]]; then
    obmm_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_dataplane_microbench=1"* ]]; then
    obmm_dataplane_microbench_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_async_coroutine=1"* ]]; then
    obmm_async_coroutine_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_import_stress=1"* ]]; then
    obmm_import_stress_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_gsva=1"* ]]; then
    obmm_gsva_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_coh_test=1"* ]]; then
    obmm_coh_test_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_mem_service_obmm_provider_conformance=1"* ]]; then
    mem_service_obmm_provider_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_gva_direct=1"* ]]; then
    gva_direct_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_gsva_query=1"* ]]; then
    gsva_query_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_npu_test=1"* ]]; then
    npu_test_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ssd_test=1"* ]]; then
    ssd_test_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ssd_gsva_test=1"* ]]; then
    ssd_gsva_test_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_mem_service=1"* ]]; then
    mem_service_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_llm_infer=1"* ]]; then
    w4_guest_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_pretraining_client_mem_service=1"* ]]; then
    pretraining_client_mem_service_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_shmem_pto_direct=1"* ]]; then
    lingqu_shmem_pto_enabled=1
  fi

  if [[ "$obmm_gsva_enabled" -eq 1 ]]; then
    append_cmdline_if_missing "obmm_gsva_mode=${OBMM_GSVA_MODE}"
    append_cmdline_if_missing "obmm_gsva_base=${OBMM_GSVA_BASE}"
    append_cmdline_if_missing "obmm_gsva_size=${OBMM_GSVA_SIZE}"
    append_cmdline_if_missing "obmm_gsva_node_count=${OBMM_GSVA_NODE_COUNT}"
    append_cmdline_if_missing "OBMM_GSVA_MODE=${OBMM_GSVA_MODE}"
    append_cmdline_if_missing "OBMM_GSVA_BASE=${OBMM_GSVA_BASE}"
    append_cmdline_if_missing "OBMM_GSVA_SIZE=${OBMM_GSVA_SIZE}"
    append_cmdline_if_missing "OBMM_GSVA_NODE_COUNT=${OBMM_GSVA_NODE_COUNT}"
  fi
  if [[ "$obmm_coh_test_enabled" -eq 1 ]]; then
    append_cmdline_if_missing "obmm_coh_test_mode=${COH_TEST_MODE}"
    append_cmdline_if_missing "obmm_coh_test_size=${COH_TEST_SIZE}"
    append_cmdline_if_missing "obmm_coh_test_iters=${COH_TEST_ITERS}"
    append_cmdline_if_missing "obmm_coh_test_node_count=2"
    append_cmdline_if_missing "obmm_coh_test_token_value=${COH_TEST_TOKEN_VALUE}"
    append_cmdline_if_missing "obmm_coh_test_generation=${COH_TEST_GENERATION}"
    append_cmdline_if_missing "obmm_coh_test_verbose=${COH_TEST_VERBOSE}"
    nodea_obmm_coh_test_append="obmm_coh_test_node_id=0 obmm_coh_test_exporter=1"
    nodeb_obmm_coh_test_append="obmm_coh_test_node_id=1"
  fi
  if [[ "$mem_service_obmm_provider_enabled" -eq 1 ]]; then
    append_cmdline_if_missing "mem_service_obmm_provider_node_count=2"
    append_cmdline_if_missing \
      "mem_service_obmm_provider_generation=${MEM_SERVICE_OBMM_PROVIDER_GENERATION}"
    nodea_mem_service_obmm_provider_append="mem_service_obmm_provider_node_id=0"
    nodeb_mem_service_obmm_provider_append="mem_service_obmm_provider_node_id=1"
  fi
  if [[ "$gva_direct_enabled" -eq 1 ]]; then
    append_cmdline_if_missing "gva_direct_mode=${GVA_DIRECT_MODE}"
    append_cmdline_if_missing "gva_direct_size=${GVA_DIRECT_SIZE}"
    append_cmdline_if_missing "gva_direct_local_va=${GVA_DIRECT_LOCAL_VA}"
    append_cmdline_if_missing "gva_direct_home_va=${GVA_DIRECT_HOME_VA}"
  fi
  if [[ "$ssd_gsva_test_enabled" -eq 1 ]]; then
    nodea_ssd_gsva_test_append="linqu_node_idx=0 linqu_node_count=2"
    nodeb_ssd_gsva_test_append="linqu_node_idx=1 linqu_node_count=2"
  fi
  if [[ "$w4_guest_enabled" -eq 1 ]]; then
    if [[ ! -f "$SIMPLER_HOST_MATMUL_MANIFEST" ]]; then
      SIMPLER_HOST_MATMUL_MANIFEST="$(ensure_simpler_host_manifest "$SCRIPT_DIR" "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" "$SIMPLER_HOST_MATMUL_MANIFEST")"
    fi
    append_cmdline_if_missing "linqu_w4_node_count=2"
    append_cmdline_if_missing "linqu_w4_all_ips=10.0.0.1,10.0.0.2"
    append_cmdline_if_missing "sim_uapi_w4_chipbackend_profile=${SIM_UAPI_W4_CHIPBACKEND_PROFILE}"
    append_cmdline_if_missing "sim_qwen3_guest_decode_steps=${SIM_QWEN3_GUEST_DECODE_STEPS}"
    nodea_w4_guest_append="linqu_w4_role=nodeA linqu_w4_local_ip=10.0.0.1"
    nodeb_w4_guest_append="linqu_w4_role=nodeB linqu_w4_local_ip=10.0.0.2"
  fi
  if [[ "$lingqu_shmem_pto_enabled" -eq 1 ]]; then
    nodea_lingqu_shmem_pto_append="linqu_node_idx=0 lingqu_shmem_pto_requester_cna=$LINGQU_SHMEM_PTO_NODEA_CNA"
    nodeb_lingqu_shmem_pto_append="linqu_node_idx=1 lingqu_shmem_pto_requester_cna=$LINGQU_SHMEM_PTO_NODEB_CNA"
  fi
  nodea_app_append="${nodea_obmm_coh_test_append} ${nodea_mem_service_obmm_provider_append} ${nodea_ssd_gsva_test_append} ${nodea_w4_guest_append} ${nodea_lingqu_shmem_pto_append}"
  nodea_app_append="${nodea_app_append#"${nodea_app_append%%[![:space:]]*}"}"
  nodea_app_append="${nodea_app_append%"${nodea_app_append##*[![:space:]]}"}"
  nodeb_app_append="${nodeb_obmm_coh_test_append} ${nodeb_mem_service_obmm_provider_append} ${nodeb_ssd_gsva_test_append} ${nodeb_w4_guest_append} ${nodeb_lingqu_shmem_pto_append}"
  nodeb_app_append="${nodeb_app_append#"${nodeb_app_append%%[![:space:]]*}"}"
  nodeb_app_append="${nodeb_app_append%"${nodeb_app_append##*[![:space:]]}"}"

  rm -f /tmp/ub-qemu/ub-bus-instance-*.lock
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

  mkdir -p "$OUT_DIR"
  mkdir -p "$LOG_DIR"
  mkdir -p "$iter_log_dir"
  mkdir -p "$SHARED_DIR"
  if [[ "$INTERACTIVE_AFTER_PASS" -eq 1 ]]; then
    mkdir -p "$SERIAL_DIR"
    rm -f "$nodea_serial" "$nodeb_serial" "$SERIAL_ENV_FILE"
  fi
  if [[ "$USE_QMP" == "1" ]]; then
    mkdir -p "$QMP_DIR"
  fi
  stale_files=(
    "$SHARED_DIR"/*.ini
    "$SHARED_DIR"/*.kick
    "$SHARED_DIR"/*.lock
    "$SHARED_DIR"/obmm_bootstrap/node*.ini
  )
  if (( ${#stale_files[@]} )); then
    rm -f "${stale_files[@]}"
  fi
  rm -f "$nodea_guest_log" "$nodeb_guest_log" "$nodea_qemu_log" "$nodeb_qemu_log"
  ln -sfn "$nodea_guest_log" "$nodea_log_link"
  ln -sfn "$nodeb_guest_log" "$nodeb_log_link"
  ln -sfn "$nodea_qemu_log" "$nodea_qemu_log_link"
  ln -sfn "$nodeb_qemu_log" "$nodeb_qemu_log_link"
  echo "iteration ${iter} logs: $iter_log_dir"

  if [[ "$USE_QMP" == "1" ]]; then
    echo "Starting nodeA (paused)..."
  else
    echo "Starting nodeA..."
  fi
  start_node "nodeA" "nodeA" "$nodea_guest_log" "$nodea_qemu_log" \
    "$nodea_pid_file" "$nodea_qmp" "$nodea_serial" "$nodea_app_append"
  sleep 0.5
  if [[ "$USE_QMP" == "1" ]]; then
    echo "Starting nodeB (paused)..."
  else
    echo "Starting nodeB..."
  fi
  start_node "nodeB" "nodeB" "$nodeb_guest_log" "$nodeb_qemu_log" \
    "$nodeb_pid_file" "$nodeb_qmp" "$nodeb_serial" "$nodeb_app_append"
  if [[ "$INTERACTIVE_AFTER_PASS" -eq 1 ]]; then
    write_serial_manifest "$nodea_serial" "$nodeb_serial"
  fi

  if ! check_link_early_or_fail "$nodea_qemu_log" "$nodeb_qemu_log" 10; then
    echo "iteration ${iter}: early link failure detected" >&2
    return 11
  fi

  if [[ "$USE_QMP" == "1" ]]; then
    cont_qemu "$nodea_qmp" "nodeA"
    cont_qemu "$nodeb_qmp" "nodeB"
  fi

  if ! wait_for_fm_links_ready "$nodea_qemu_log" "$nodeb_qemu_log" 30; then
    echo "iteration ${iter}: FM links failed to reach READY state within timeout" >&2
    return 11
  fi

  if [ "$ENTITY_COUNT" -gt "1" ]; then
    if ! check_entity_ready "nodeA" "$nodea_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeA entities not ready within timeout" >&2
      return 12
    fi
    if ! check_entity_ready "nodeB" "$nodeb_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeB entities not ready within timeout" >&2
      return 12
    fi
  fi

  if (( lingqu_shmem_pto_enabled == 1 &&
        LINGQU_SHMEM_PTO_RESET_ON_PENDING != 0 )); then
    if ! wait_for_log_pattern "$nodeb_qemu_log" \
         "QEMU_UB_GM_AUTHORIZATION_PENDING .*cursor=0 .*sequence=[1-9][0-9]*" \
         30; then
      echo "iteration ${iter}: PTO authorization did not suspend before reset" >&2
      return 31
    fi
    echo "Resetting nodeB after PTO authorization suspended..."
    if ! qmp_execute_strict "$nodeb_qmp" system_reset nodeB; then
      echo "iteration ${iter}: nodeB QMP reset failed" >&2
      return 31
    fi
    if ! wait_for_log_pattern "$nodeb_qemu_log" \
         "QEMU_UB_GM_RESET authorization_pending=1 .*cq_completion=0" 10; then
      echo "iteration ${iter}: nodeB reset did not discard pending authorization" >&2
      return 31
    fi
  fi

  sleep 1
  if ! kill -0 "$(cat "$nodea_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeA died after resume" >&2
    return 1
  fi
  if ! kill -0 "$(cat "$nodeb_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeB died after resume" >&2
    return 1
  fi

  if [[ "$lingqu_shmem_pto_enabled" -eq 1 ]]; then
    local nodea_expected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=pass"
    local nodea_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=fail"
    local nodeb_expected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=pass"
    local nodeb_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=fail"

    if [[ "$LINGQU_SHMEM_PTO_EXPECT" == "authorization-timeout" ]]; then
      nodea_expected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=pass expected=authorization-timeout observed=verify_timeout"
      nodea_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=fail"
      nodeb_expected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=pass expected=authorization-timeout observed=completion error=pto_ub_gm_authorization_timeout"
      nodeb_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=fail"
    elif [[ "$LINGQU_SHMEM_PTO_EXPECT" == "authorization-cancelled" ]]; then
      nodea_expected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=pass expected=authorization-cancelled observed=verify_timeout"
      nodea_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=producer status=fail"
      nodeb_expected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=pass expected=authorization-cancelled observed=completion error=pto_ub_gm_authorization_cancelled"
      nodeb_unexpected_result="LINGQU_SHMEM_PTO_RESULT role=consumer status=fail"
    fi

    if wait_for_log_pass_or_fail "$nodea_guest_log" \
         "$nodea_expected_result" "$nodea_unexpected_result" "$RUN_SECS"; then
      wait_status=0
    else
      wait_status=$?
    fi
    case "$wait_status" in
      0) ;;
      1)
        echo "iteration ${iter}: PTO producer reported unexpected result" >&2
        return 30
        ;;
      *)
        echo "iteration ${iter}: PTO producer did not finish within ${RUN_SECS}s" >&2
        return 30
        ;;
    esac

    if wait_for_log_pass_or_fail "$nodeb_guest_log" \
         "$nodeb_expected_result" "$nodeb_unexpected_result" "$RUN_SECS"; then
      wait_status=0
    else
      wait_status=$?
    fi
    case "$wait_status" in
      0) ;;
      1)
        echo "iteration ${iter}: PTO consumer reported unexpected result" >&2
        return 30
        ;;
      *)
        echo "iteration ${iter}: PTO consumer did not finish within ${RUN_SECS}s" >&2
        return 30
        ;;
    esac
  fi

  if [[ "$chat_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ub_chat\\] pass" "\\[ub_chat\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA chat app reported failure" >&2
        return 15
        ;;
      *)
        echo "iteration ${iter}: nodeA chat app did not pass within ${RUN_SECS}s" >&2
        return 15
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ub_chat\\] pass" "\\[ub_chat\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB chat app reported failure" >&2
        return 15
        ;;
      *)
        echo "iteration ${iter}: nodeB chat app did not pass within ${RUN_SECS}s" >&2
        return 15
        ;;
    esac
  fi

  if [[ "$rpc_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ub_rpc\\] pass" "\\[ub_rpc\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA rpc app reported failure" >&2
        return 13
        ;;
      *)
        echo "iteration ${iter}: nodeA rpc app did not pass within ${RUN_SECS}s" >&2
        return 13
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ub_rpc\\] pass" "\\[ub_rpc\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB rpc app reported failure" >&2
        return 13
        ;;
      *)
        echo "iteration ${iter}: nodeB rpc app did not pass within ${RUN_SECS}s" >&2
        return 13
        ;;
    esac
  fi

  if [[ "$tcp_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ub_tcp_each_server\\] pass" "\\[ub_tcp_each_server\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA tcp each server app reported failure" >&2
        return 17
        ;;
      *)
        echo "iteration ${iter}: nodeA tcp each server app did not pass within ${RUN_SECS}s" >&2
        return 17
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ub_tcp_each_server\\] pass" "\\[ub_tcp_each_server\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB tcp each server app reported failure" >&2
        return 17
        ;;
      *)
        echo "iteration ${iter}: nodeB tcp each server app did not pass within ${RUN_SECS}s" >&2
        return 17
        ;;
    esac
  fi

  if [[ "$udma_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ub_udma\\] pass" "\\[ub_udma\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA udma app reported failure" >&2
        return 14
        ;;
      *)
        echo "iteration ${iter}: nodeA udma app did not finish within ${RUN_SECS}s" >&2
        return 14
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ub_udma\\] pass" "\\[ub_udma\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB udma app reported failure" >&2
        return 14
        ;;
      *)
        echo "iteration ${iter}: nodeB udma app did not finish within ${RUN_SECS}s" >&2
        return 14
        ;;
    esac
  fi

  if [[ "$obmm_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ub_obmm_pool\\] pass" "\\[ub_obmm_pool\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm pool app reported failure" >&2
        return 16
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm pool app did not finish within ${RUN_SECS}s" >&2
        return 16
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ub_obmm_pool\\] pass" "\\[ub_obmm_pool\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm pool app reported failure" >&2
        return 16
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm pool app did not finish within ${RUN_SECS}s" >&2
        return 16
        ;;
    esac
  fi

  if [[ "$obmm_dataplane_microbench_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[obmm_dataplane_microbench\\] result=done" \
      "\\[obmm_dataplane_microbench\\].*(result=fail|bench failed|verify_failures=[1-9])" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm dataplane microbench app reported failure" >&2
        return 18
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm dataplane microbench app did not finish within ${RUN_SECS}s" >&2
        return 18
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[obmm_dataplane_microbench\\] result=done" \
      "\\[obmm_dataplane_microbench\\].*(result=fail|bench failed|verify_failures=[1-9])" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm dataplane microbench app reported failure" >&2
        return 18
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm dataplane microbench app did not finish within ${RUN_SECS}s" >&2
        return 18
        ;;
    esac
  fi

  if [[ "$obmm_async_coroutine_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" \
      "OBMM_ASYNC_SUMMARY .*status=pass" \
      "OBMM_ASYNC_SUMMARY .*status=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA OBMM async app reported failure" >&2
        return 31
        ;;
      *)
        echo "iteration ${iter}: nodeA OBMM async app timed out" >&2
        return 31
        ;;
    esac
    wait_for_log_pass_or_fail "$nodeb_guest_log" \
      "OBMM_ASYNC_SUMMARY .*status=pass" \
      "OBMM_ASYNC_SUMMARY .*status=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB OBMM async app reported failure" >&2
        return 31
        ;;
      *)
        echo "iteration ${iter}: nodeB OBMM async app timed out" >&2
        return 31
        ;;
    esac
  fi

  if [[ "$obmm_import_stress_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[obmm_import_stress\\] result=done" \
      "\\[obmm_import_stress\\] stress_run failed" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm import stress app reported failure" >&2
        return 19
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm import stress app did not finish within ${RUN_SECS}s" >&2
        return 19
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[obmm_import_stress\\] result=done" \
      "\\[obmm_import_stress\\] stress_run failed" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm import stress app reported failure" >&2
        return 19
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm import stress app did not finish within ${RUN_SECS}s" >&2
        return 19
        ;;
    esac
  fi

  if [[ "$obmm_gsva_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[obmm_gsva\\] result=done" \
      "\\[obmm_gsva\\] result=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm gsva app reported failure" >&2
        return 20
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm gsva app did not finish within ${RUN_SECS}s" >&2
        return 20
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[obmm_gsva\\] result=done" \
      "\\[obmm_gsva\\] result=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm gsva app reported failure" >&2
        return 20
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm gsva app did not finish within ${RUN_SECS}s" >&2
        return 20
        ;;
    esac
  fi

  if [[ "$obmm_coh_test_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "obmm_coh_test: PASS" \
      "obmm_coh_test: FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm coh test app reported failure" >&2
        return 21
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm coh test app did not finish within ${RUN_SECS}s" >&2
        return 21
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "obmm_coh_test: PASS" \
      "obmm_coh_test: FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm coh test app reported failure" >&2
        return 21
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm coh test app did not finish within ${RUN_SECS}s" >&2
        return 21
        ;;
    esac
  fi

  if [[ "$mem_service_obmm_provider_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" \
      "mem_service obmm-provider-conformance: status=ok node=0" \
      "mem_service obmm-provider-conformance: status=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA OBMM provider conformance failed" >&2
        return 30
        ;;
      *)
        echo "iteration ${iter}: nodeA OBMM provider conformance timed out" >&2
        return 30
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" \
      "mem_service obmm-provider-conformance: status=ok node=1" \
      "mem_service obmm-provider-conformance: status=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB OBMM provider conformance failed" >&2
        return 30
        ;;
      *)
        echo "iteration ${iter}: nodeB OBMM provider conformance timed out" >&2
        return 30
        ;;
    esac
  fi

  if [[ "$gva_direct_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[gva_direct\\] result=done" \
      "\\[gva_direct\\] result=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA gva direct app reported failure" >&2
        return 22
        ;;
      *)
        echo "iteration ${iter}: nodeA gva direct app did not finish within ${RUN_SECS}s" >&2
        return 22
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[gva_direct\\] result=done" \
      "\\[gva_direct\\] result=fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB gva direct app reported failure" >&2
        return 22
        ;;
      *)
        echo "iteration ${iter}: nodeB gva direct app did not finish within ${RUN_SECS}s" >&2
        return 22
        ;;
    esac
  fi

  if [[ "$gsva_query_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "verdict=PASS" \
      "verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA gsva query app reported failure" >&2
        return 24
        ;;
      *)
        echo "iteration ${iter}: nodeA gsva query app did not finish within ${RUN_SECS}s" >&2
        return 24
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "verdict=PASS" \
      "verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB gsva query app reported failure" >&2
        return 24
        ;;
      *)
        echo "iteration ${iter}: nodeB gsva query app did not finish within ${RUN_SECS}s" >&2
        return 24
        ;;
    esac
  fi

  if [[ "$npu_test_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[npu_test\\] verdict=(PASS|SKIP)" \
      "\\[npu_test\\] verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA npu test app reported failure" >&2
        return 23
        ;;
      *)
        echo "iteration ${iter}: nodeA npu test app did not finish within ${RUN_SECS}s" >&2
        return 23
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[npu_test\\] verdict=(PASS|SKIP)" \
      "\\[npu_test\\] verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB npu test app reported failure" >&2
        return 23
        ;;
      *)
        echo "iteration ${iter}: nodeB npu test app did not finish within ${RUN_SECS}s" >&2
        return 23
        ;;
    esac
  fi

  if [[ "$ssd_test_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ssd_test\\] verdict=PASS" \
      "\\[ssd_test\\] verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA ssd test app reported failure" >&2
        return 25
        ;;
      *)
        echo "iteration ${iter}: nodeA ssd test app did not finish within ${RUN_SECS}s" >&2
        return 25
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ssd_test\\] verdict=PASS" \
      "\\[ssd_test\\] verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB ssd test app reported failure" >&2
        return 25
        ;;
      *)
        echo "iteration ${iter}: nodeB ssd test app did not finish within ${RUN_SECS}s" >&2
        return 25
        ;;
    esac
  fi

  if [[ "$ssd_gsva_test_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[ssd_gsva_test\\]verdict=PASS" \
      "\\[ssd_gsva_test\\]verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA ssd gsva test app reported failure" >&2
        return 26
        ;;
      *)
        echo "iteration ${iter}: nodeA ssd gsva test app did not finish within ${RUN_SECS}s" >&2
        return 26
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[ssd_gsva_test\\]verdict=PASS" \
      "\\[ssd_gsva_test\\]verdict=FAIL" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB ssd gsva test app reported failure" >&2
        return 26
        ;;
      *)
        echo "iteration ${iter}: nodeB ssd gsva test app did not finish within ${RUN_SECS}s" >&2
        return 26
        ;;
    esac
  fi

  if [[ "$mem_service_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "mem_service smoke: status=ok" \
      "mem_service smoke: .* failed" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA mem_service smoke reported failure" >&2
        return 27
        ;;
      *)
        echo "iteration ${iter}: nodeA mem_service smoke did not finish within ${RUN_SECS}s" >&2
        return 27
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "mem_service smoke: status=ok" \
      "mem_service smoke: .* failed" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB mem_service smoke reported failure" >&2
        return 27
        ;;
      *)
        echo "iteration ${iter}: nodeB mem_service smoke did not finish within ${RUN_SECS}s" >&2
        return 27
        ;;
    esac
  fi

  if [[ "$pretraining_client_mem_service_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" \
      "linqu_pretraining_client mem_service publish restart verify done" \
      "linqu_pretraining_client_mem_service_.* failed|pretraining record mismatch|expected not found" \
      "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA pretraining client mem_service app reported failure" >&2
        return 29
        ;;
      *)
        echo "iteration ${iter}: nodeA pretraining client mem_service app did not finish within ${RUN_SECS}s" >&2
        return 29
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" \
      "linqu_pretraining_client mem_service publish restart verify done" \
      "linqu_pretraining_client_mem_service_.* failed|pretraining record mismatch|expected not found" \
      "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB pretraining client mem_service app reported failure" >&2
        return 29
        ;;
      *)
        echo "iteration ${iter}: nodeB pretraining client mem_service app did not finish within ${RUN_SECS}s" >&2
        return 29
        ;;
    esac
  fi

  if [[ "$w4_guest_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[w4_guest\\] pass" \
      "\\[w4_guest\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA w4 guest app reported failure" >&2
        return 28
        ;;
      *)
        echo "iteration ${iter}: nodeA w4 guest app did not finish within ${RUN_SECS}s" >&2
        return 28
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[w4_guest\\] pass" \
      "\\[w4_guest\\] fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB w4 guest app reported failure" >&2
        return 28
        ;;
      *)
        echo "iteration ${iter}: nodeB w4 guest app did not finish within ${RUN_SECS}s" >&2
        return 28
        ;;
    esac
  fi

  sleep 1
  if [[ "$INTERACTIVE_AFTER_PASS" -eq 0 ]]; then
    cleanup_pid "$nodea_pid_file"
    cleanup_pid "$nodeb_pid_file"
  fi

  echo "=== nodeA guest(apps:${iter}) ==="
  tail -n 120 "$nodea_guest_log"
  echo "=== nodeB guest(apps:${iter}) ==="
  tail -n 120 "$nodeb_guest_log"
  echo "=== nodeA qemu(apps:${iter}) ==="
  tail -n 80 "$nodea_qemu_log"
  echo "=== nodeB qemu(apps:${iter}) ==="
  tail -n 80 "$nodeb_qemu_log"

  if [[ "$chat_enabled" -eq 1 ]]; then
    validate_chat_log "nodeA" "$nodea_guest_log" || return 1
    validate_chat_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$rpc_enabled" -eq 1 ]]; then
    validate_rpc_log "nodeA" "$nodea_guest_log" || return 1
    validate_rpc_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$tcp_enabled" -eq 1 ]]; then
    validate_tcp_each_server_log "nodeA" "$nodea_guest_log" || return 1
    validate_tcp_each_server_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_udma=1"* ]]; then
    validate_udma_log "nodeA" "$nodea_guest_log" || return 1
    validate_udma_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_pool=1"* ]]; then
    validate_obmm_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_dataplane_microbench=1"* ]]; then
    validate_obmm_dataplane_microbench_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_dataplane_microbench_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_async_coroutine=1"* ]]; then
    validate_obmm_async_coroutine_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_async_coroutine_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_import_stress=1"* ]]; then
    validate_obmm_import_stress_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_import_stress_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_gsva=1"* ]]; then
    validate_obmm_gsva_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_gsva_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_coh_test=1"* ]]; then
    validate_obmm_coh_test_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_coh_test_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_mem_service_obmm_provider_conformance=1"* ]]; then
    validate_mem_service_obmm_provider_log \
      "nodeA" "$nodea_guest_log" || return 1
    validate_mem_service_obmm_provider_log \
      "nodeB" "$nodeb_guest_log" || return 1
    validate_mem_service_obmm_provider_qemu_log \
      "nodeA" "$nodea_qemu_log" || return 1
    validate_mem_service_obmm_provider_qemu_log \
      "nodeB" "$nodeb_qemu_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_gva_direct=1"* ]]; then
    validate_gva_direct_log "nodeA" "$nodea_guest_log" || return 1
    validate_gva_direct_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_gsva_query=1"* ]]; then
    validate_gsva_query_log "nodeA" "$nodea_guest_log" || return 1
    validate_gsva_query_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_npu_test=1"* ]]; then
    validate_npu_test_log "nodeA" "$nodea_guest_log" || return 1
    validate_npu_test_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ssd_test=1"* ]]; then
    validate_ssd_test_log "nodeA" "$nodea_guest_log" || return 1
    validate_ssd_test_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ssd_gsva_test=1"* ]]; then
    validate_ssd_gsva_test_log "nodeA" "$nodea_guest_log" || return 1
    validate_ssd_gsva_test_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_mem_service=1"* ]]; then
    validate_mem_service_log "nodeA" "$nodea_guest_log" || return 1
    validate_mem_service_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_llm_infer=1"* ]]; then
    validate_w4_guest_log "nodeA" "$nodea_guest_log" || return 1
    validate_w4_guest_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$lingqu_shmem_pto_enabled" -eq 1 ]]; then
    validate_lingqu_shmem_pto_guest_log \
      "producer" "$nodea_guest_log" || return 1
    validate_lingqu_shmem_pto_guest_log \
      "consumer" "$nodeb_guest_log" || return 1
    validate_lingqu_shmem_pto_qemu_log \
      "nodeA" "$nodea_qemu_log" "$nodea_guest_log" || return 1
    validate_lingqu_shmem_pto_qemu_log \
      "nodeB" "$nodeb_qemu_log" "$nodea_guest_log" || return 1
  fi
  validate_kernel_health_log "nodeA" "$nodea_guest_log" || return 1
  validate_kernel_health_log "nodeB" "$nodeb_guest_log" || return 1

  echo "iteration ${iter}: dual-node apps pass"
  if [[ "$INTERACTIVE_AFTER_PASS" -eq 1 ]]; then
    cleanup_watchdog
    echo "interactive shells ready; use node input or Stop to terminate"
    echo "serial manifest: $SERIAL_ENV_FILE"
    while kill -0 "$(cat "$nodea_pid_file" 2>/dev/null)" 2>/dev/null &&
          kill -0 "$(cat "$nodeb_pid_file" 2>/dev/null)" 2>/dev/null; do
      sleep 1
    done
    echo "interactive guest exited before Stop" >&2
    return 29
  fi
}

declare -a ITERATION_RESULTS
declare -a ITERATION_ERRORS

for ((i = 1; i <= ITERATIONS; i++)); do
  if run_iteration "$i"; then
    ITERATION_RESULTS[$i]=0
  else
    ret=$?
    ITERATION_RESULTS[$i]=$ret
    ITERATION_ERRORS[$i]="iteration $i failed with exit code $ret"
  fi
done

echo "=== Test Results ===" >&2
passed=0
failed=0
for ((i = 1; i <= ITERATIONS; i++)); do
  if [[ ${ITERATION_RESULTS[$i]:-255} -eq 0 ]]; then
    passed=$((passed + 1))
    echo "iteration $i: PASS"
  else
    failed=$((failed + 1))
    echo "iteration $i: FAIL (exit code ${ITERATION_RESULTS[$i]})" >&2
    if [[ -n "${ITERATION_ERRORS[$i]}" ]]; then
      echo "  ${ITERATION_ERRORS[$i]}" >&2
    fi
  fi
done

echo "=== Summary ===" >&2
echo "Passed: $passed / $ITERATIONS" >&2
echo "Failed: $failed / $ITERATIONS" >&2

pass_rate=$((passed * 100 / ITERATIONS))
echo "Pass rate: ${pass_rate}% (required >= ${MIN_PASS_RATE_PERCENT}%)" >&2

{
  echo "scenario=dual-node-apps"
  echo "iterations=${ITERATIONS}"
  echo "run_secs=${RUN_SECS}"
  echo "start_gap_secs=${START_GAP_SECS}"
  echo "run_id=${RUN_ID}"
  echo "logs_dir=${LOG_DIR}"
  echo "min_pass_rate_percent=${MIN_PASS_RATE_PERCENT}"
  echo "max_runtime=${MAX_RUNTIME}"
  if [[ "$APPEND_EXTRA" == *"linqu_shmem_pto_direct=1"* ]]; then
    echo "lingqu_shmem_pto_manifest=${SIMPLER_HOST_VECTOR_MANIFEST}"
    echo "lingqu_shmem_pto_artifact_fingerprint=${LINGQU_SHMEM_PTO_ARTIFACT_FINGERPRINT}"
    echo "lingqu_shmem_pto_layout=${LINGQU_SHMEM_PTO_LAYOUT}"
    echo "lingqu_shmem_pto_elements=${LINGQU_SHMEM_PTO_ELEMENTS}"
    echo "lingqu_shmem_pto_generation=${LINGQU_SHMEM_PTO_GENERATION}"
    echo "lingqu_shmem_pto_nodea_cna=${LINGQU_SHMEM_PTO_NODEA_CNA}"
    echo "lingqu_shmem_pto_nodeb_cna=${LINGQU_SHMEM_PTO_NODEB_CNA}"
    echo "lingqu_shmem_pto_authorization_delay_ns=${LINGQU_SHMEM_PTO_AUTHORIZATION_DELAY_NS}"
    echo "lingqu_shmem_pto_authorization_timeout_ns=${LINGQU_SHMEM_PTO_AUTHORIZATION_TIMEOUT_NS}"
    echo "lingqu_shmem_pto_cancel_after_ms=${LINGQU_SHMEM_PTO_CANCEL_AFTER_MS}"
    echo "lingqu_shmem_pto_reset_on_pending=${LINGQU_SHMEM_PTO_RESET_ON_PENDING}"
    echo "lingqu_shmem_pto_inject_duplicate_completion=${LINGQU_SHMEM_PTO_INJECT_DUPLICATE_COMPLETION}"
    echo "lingqu_shmem_pto_inject_late_completion=${LINGQU_SHMEM_PTO_INJECT_LATE_COMPLETION}"
    echo "lingqu_shmem_pto_expect=${LINGQU_SHMEM_PTO_EXPECT}"
    echo "lingqu_shmem_pto_fault_case=${LINGQU_SHMEM_PTO_FAULT_CASE}"
  fi
  echo "passed=${passed}"
  echo "failed=${failed}"
  echo "pass_rate_percent=${pass_rate}"
  for ((i = 1; i <= ITERATIONS; i++)); do
    echo "iteration_${i}_result=${ITERATION_RESULTS[$i]:-255}"
  done
} > "$REPORT_FILE"
echo "Report: $REPORT_FILE" >&2

if (( pass_rate < MIN_PASS_RATE_PERCENT )); then
  echo "dual-node apps validation FAILED" >&2
  exit 1
fi

echo "dual-node apps validation passed (${ITERATIONS} iterations)"
exit 0
