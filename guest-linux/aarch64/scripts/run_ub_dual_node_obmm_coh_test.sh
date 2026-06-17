#!/bin/zsh
set -euo pipefail
setopt null_glob

# OBMM Directory MESI Coherence Test - Dual Node
#
# Runs linqu_ub_obmm_coh_test automatically on a 2-node QEMU setup.
# Node A exports memory; node B imports it with cache_policy=DIRECTORY_MESI.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-coh}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
RUN_SECS="${RUN_SECS:-180}"
QMP_MODE="${QMP_MODE:-auto}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_coh_${RANDOM}}"

COH_TEST_MODE="${COH_TEST_MODE:-write_read}"
COH_TEST_SIZE="${COH_TEST_SIZE:-2097152}"
COH_TEST_ITERS="${COH_TEST_ITERS:-1}"
COH_TEST_TOKEN_VALUE="${COH_TEST_TOKEN_VALUE:-0}"
COH_TEST_GENERATION="${COH_TEST_GENERATION:-1}"
COH_TEST_VERBOSE="${COH_TEST_VERBOSE:-1}"
COH_REQUIRE_LOGS="${COH_REQUIRE_LOGS:-1}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      COH_TEST_MODE="$2"
      shift 2
      ;;
    --size)
      COH_TEST_SIZE="$2"
      shift 2
      ;;
    --iterations)
      COH_TEST_ITERS="$2"
      shift 2
      ;;
    --generation)
      COH_TEST_GENERATION="$2"
      shift 2
      ;;
    --keep)
      QEMU_KEEP_ALIVE_ON_POWEROFF=1
      shift
      ;;
    --help)
      echo "Usage: $0 [--mode <test>] [--size <bytes>] [--iterations <n>] [--generation <n>] [--keep]"
      exit 0
      ;;
    *)
      echo "[coh_test] unknown option: $1" >&2
      exit 1
      ;;
  esac
done

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

mkdir -p "$LOG_DIR/${RUN_ID}"
NODEA_GUEST_LOG="$LOG_DIR/${RUN_ID}/nodeA_guest.log"
NODEB_GUEST_LOG="$LOG_DIR/${RUN_ID}/nodeB_guest.log"
NODEA_QEMU_LOG="$LOG_DIR/${RUN_ID}/nodeA_qemu.log"
NODEB_QEMU_LOG="$LOG_DIR/${RUN_ID}/nodeB_qemu.log"
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.coh.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.coh.${RUN_ID}.pid"
NODEA_QMP="$SHARED_DIR/qmp/nodeA.qmp"
NODEB_QMP="$SHARED_DIR/qmp/nodeB.qmp"

cleanup() {
  local pid_file
  for pid_file in "$NODEA_PID_FILE" "$NODEB_PID_FILE"; do
    if [[ -f "$pid_file" ]]; then
      local pid
      pid="$(cat "$pid_file" 2>/dev/null || true)"
      if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        sleep 0.5
        kill -9 "$pid" 2>/dev/null || true
      fi
      rm -f "$pid_file"
    fi
  done
}
trap 'cleanup' EXIT INT TERM

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR/qmp"

start_node() {
  local node_id="$1"
  local role="$2"
  local coh_node_id="$3"
  local exporter="$4"
  local guest_log="$5"
  local qemu_log="$6"
  local pid_file="$7"
  local qmp_socket="$8"
  local qemu_extra=()
  local qmp_flag=()
  local exporter_arg=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi
  if [[ "$QMP_MODE" == "on" ]]; then
    qmp_flag=(-qmp unix:"$qmp_socket",server=on,wait=off)
  fi
  if [[ "$exporter" == "1" ]]; then
    exporter_arg="obmm_coh_test_exporter=1"
  fi

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    "$QEMU_BIN" \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -m 8G \
      -nodefaults \
      -nographic \
      ${qmp_flag[@]} \
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo linqu_obmm_coh_test=1 linqu_urma_dp_role=${role} obmm_coh_test_mode=${COH_TEST_MODE} obmm_coh_test_size=${COH_TEST_SIZE} obmm_coh_test_iters=${COH_TEST_ITERS} obmm_coh_test_node_id=${coh_node_id} obmm_coh_test_node_count=2 obmm_coh_test_token_value=${COH_TEST_TOKEN_VALUE} obmm_coh_test_generation=${COH_TEST_GENERATION} obmm_coh_test_verbose=${COH_TEST_VERBOSE} ${exporter_arg} ${APPEND_EXTRA}" \
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

  while (( SECONDS < deadline )); do
    local nodea_ready=false
    local nodeb_ready=false
    if [[ -f "$nodea_status" ]] && grep -q "^state=READY" "$nodea_status"; then
      nodea_ready=true
    fi
    if [[ "$nodea_ready" == "false" ]] && [[ -f "$nodea_log" ]] && \
       grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodea_log"; then
      nodea_ready=true
    fi
    if [[ -f "$nodeb_status" ]] && grep -q "^state=READY" "$nodeb_status"; then
      nodeb_ready=true
    fi
    if [[ "$nodeb_ready" == "false" ]] && [[ -f "$nodeb_log" ]] && \
       grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodeb_log"; then
      nodeb_ready=true
    fi
    if [[ "$nodea_ready" == "true" && "$nodeb_ready" == "true" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

validate_coh_logs() {
  if [[ "$COH_REQUIRE_LOGS" != "1" ]]; then
    return 0
  fi

  if ! grep -Eq 'GVA_S3_MAP .*cache_policy=4' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
    echo "[coh_test] FAIL: missing directory-MESI cache_policy=4 map evidence" >&2
    return 1
  fi
  case "$COH_TEST_MODE" in
    write_read)
      if ! grep -Eq 'OBMM_COH_GETS' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: write_read missing OBMM_COH_GETS evidence" >&2
        return 1
      fi
      ;;
    fence|read_after_wb)
      if ! grep -Eq 'OBMM_COH_FENCE' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: $COH_TEST_MODE missing OBMM_COH_FENCE evidence" >&2
        return 1
      fi
      if [[ "$COH_TEST_MODE" == "read_after_wb" ]] &&
         ! grep -Eq 'OBMM_COH_WB' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: read_after_wb missing OBMM_COH_WB evidence" >&2
        return 1
      fi
      ;;
    all)
      if ! grep -Eq 'OBMM_COH_GETS' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: all missing OBMM_COH_GETS evidence" >&2
        return 1
      fi
      if ! grep -Eq 'OBMM_COH_FENCE' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: all missing OBMM_COH_FENCE evidence" >&2
        return 1
      fi
      if ! grep -Eq 'OBMM_COH_WB' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: all missing OBMM_COH_WB evidence" >&2
        return 1
      fi
      ;;
    *)
      if ! grep -Eq 'OBMM_COH_GETS|OBMM_COH_FENCE' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[coh_test] FAIL: missing OBMM_COH smoke/fence evidence" >&2
        return 1
      fi
      ;;
  esac
}

echo "[coh_test] run_id=$RUN_ID mode=$COH_TEST_MODE size=$COH_TEST_SIZE iterations=$COH_TEST_ITERS"
echo "[coh_test] starting nodeA exporter and nodeB importer..."

start_node nodeA nodeA 0 1 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE" "$NODEA_QMP"
start_node nodeB nodeB 1 0 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE" "$NODEB_QMP"

echo "[coh_test] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[coh_test] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[coh_test] FM links ready"

echo "[coh_test] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -q 'obmm_coh_test: PASS' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -q 'obmm_coh_test: PASS' "$NODEB_GUEST_LOG"; then
    cleanup
    sleep 0.5
    validate_coh_logs
    echo "[coh_test] PASS: both nodes completed"
    exit 0
  fi
  if grep -qE 'obmm_coh_test: FAIL|\[run_demo\] linqu_ub_obmm_coh_test failed|Kernel panic - not syncing|Call trace:' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[coh_test] FAIL: guest reported coherence test failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[coh_test] FAIL: timeout waiting for completion" >&2
exit 1
