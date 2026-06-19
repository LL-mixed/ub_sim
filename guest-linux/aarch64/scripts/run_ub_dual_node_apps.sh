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
GVA_DIRECT_MODE="${GVA_DIRECT_MODE:-write-read}"
GVA_DIRECT_LOCAL_VA="${GVA_DIRECT_LOCAL_VA:-0x710000000000}"
GVA_DIRECT_HOME_VA="${GVA_DIRECT_HOME_VA:-0x720000000000}"
GVA_DIRECT_SIZE="${GVA_DIRECT_SIZE:-0x400000}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-host_matmul}"
SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
SIMPLER_HOST_VECTOR_MANIFEST="${SIMPLER_HOST_VECTOR_MANIFEST:-/tmp/simpler-host-vector-artifacts/host_vector_manifest.json}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIM_UAPI_SCENARIO_CONFIG="${SIM_UAPI_SCENARIO_CONFIG:-$WORKSPACE_ROOT/scenarios/mvp_4host_single_domain.yaml}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
QMP_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}/qmp"
MIN_PASS_RATE_PERCENT="${MIN_PASS_RATE_PERCENT:-100}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/apps_report.latest.txt}"
MAX_RUNTIME="${MAX_RUNTIME:-300}"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_${RANDOM}}"
MAIN_PID=$$

usage() {
  cat <<'USAGE'
Usage: run_ub_dual_node_apps.sh [options]

Options:
  --app NAME          App to validate. Repeat or pass comma-separated names.
                      Names: chat, rpc, tcp_each_server, udma, obmm_pool,
                      obmm_dataplane_microbench, obmm_import_stress, obmm_gsva,
                      obmm_coh_test, gva_direct, gsva_query, npu_test, ssd_test,
                      ssd_gsva_test, mem_service, llm_infer.
                      Default: chat,rpc,tcp_each_server.
  --run-id ID        Stable run id used for log/report names.
  --run-secs SECS    Per-app pass/fail wait timeout.
  --iterations N     Number of dual-node iterations.
  --max-runtime SECS Global watchdog timeout.
  --append-extra STR Extra kernel cmdline tokens to append.
  --use-qmp          Start guests paused and resume them through QMP.
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
    obmm_import_stress)
      flag="linqu_obmm_import_stress=1"
      ;;
    obmm_gsva)
      flag="linqu_obmm_gsva=1"
      ;;
    obmm_coh_test)
      flag="linqu_obmm_coh_test=1"
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
    --append-extra)
      if [[ $# -lt 2 ]]; then
        echo "--append-extra requires a value" >&2
        usage >&2
        exit 2
      fi
      APPEND_EXTRA="${APPEND_EXTRA} $2"
      shift 2
      ;;
    --use-qmp)
      USE_QMP=1
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

apply_app_selection "$APP_SELECTION"

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
  sleep "$timeout_sec"
  echo "global timeout ${timeout_sec}s reached, terminating test" >&2
  kill -TERM "$MAIN_PID" 2>/dev/null || true
}

timeout_watchdog "$MAX_RUNTIME" &
WATCHDOG_PID=$!
trap 'kill "$WATCHDOG_PID" 2>/dev/null || true; cleanup_all_app_pid_files' EXIT INT TERM

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
  local app_append_extra="${7-}"
  local qemu_extra=()
  local qemu_control_args=()
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
  mkdir -p "$(dirname "$guest_log")"
  mkdir -p "$(dirname "$qemu_log")"

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
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
      -serial file:"$guest_log" \
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
  local chat_enabled=0
  local rpc_enabled=0
  local tcp_enabled=0
  local udma_enabled=0
  local obmm_enabled=0
  local obmm_dataplane_microbench_enabled=0
  local obmm_import_stress_enabled=0
  local obmm_gsva_enabled=0
  local obmm_coh_test_enabled=0
  local gva_direct_enabled=0
  local gsva_query_enabled=0
  local npu_test_enabled=0
  local ssd_test_enabled=0
  local ssd_gsva_test_enabled=0
  local mem_service_enabled=0
  local w4_guest_enabled=0
  local nodea_obmm_coh_test_append=""
  local nodeb_obmm_coh_test_append=""
  local nodea_ssd_gsva_test_append=""
  local nodeb_ssd_gsva_test_append=""
  local nodea_w4_guest_append=""
  local nodeb_w4_guest_append=""
  local nodea_app_append=""
  local nodeb_app_append=""
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
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_import_stress=1"* ]]; then
    obmm_import_stress_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_gsva=1"* ]]; then
    obmm_gsva_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_coh_test=1"* ]]; then
    obmm_coh_test_enabled=1
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
  nodea_app_append="${nodea_obmm_coh_test_append} ${nodea_ssd_gsva_test_append} ${nodea_w4_guest_append}"
  nodea_app_append="${nodea_app_append#"${nodea_app_append%%[![:space:]]*}"}"
  nodea_app_append="${nodea_app_append%"${nodea_app_append##*[![:space:]]}"}"
  nodeb_app_append="${nodeb_obmm_coh_test_append} ${nodeb_ssd_gsva_test_append} ${nodeb_w4_guest_append}"
  nodeb_app_append="${nodeb_app_append#"${nodeb_app_append%%[![:space:]]*}"}"
  nodeb_app_append="${nodeb_app_append%"${nodeb_app_append##*[![:space:]]}"}"

  rm -f /tmp/ub-qemu/ub-bus-instance-*.lock
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

  mkdir -p "$OUT_DIR"
  mkdir -p "$LOG_DIR"
  mkdir -p "$iter_log_dir"
  mkdir -p "$SHARED_DIR"
  if [[ "$USE_QMP" == "1" ]]; then
    mkdir -p "$QMP_DIR"
  fi
  stale_files=("$SHARED_DIR"/*.ini "$SHARED_DIR"/*.kick "$SHARED_DIR"/*.lock)
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
    "$nodea_pid_file" "$nodea_qmp" "$nodea_app_append"
  sleep 0.5
  if [[ "$USE_QMP" == "1" ]]; then
    echo "Starting nodeB (paused)..."
  else
    echo "Starting nodeB..."
  fi
  start_node "nodeB" "nodeB" "$nodeb_guest_log" "$nodeb_qemu_log" \
    "$nodeb_pid_file" "$nodeb_qmp" "$nodeb_app_append"

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

  sleep 1
  if ! kill -0 "$(cat "$nodea_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeA died after resume" >&2
    return 1
  fi
  if ! kill -0 "$(cat "$nodeb_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeB died after resume" >&2
    return 1
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
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

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
  validate_kernel_health_log "nodeA" "$nodea_guest_log" || return 1
  validate_kernel_health_log "nodeB" "$nodeb_guest_log" || return 1

  echo "iteration ${iter}: dual-node apps pass"
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
