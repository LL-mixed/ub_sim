#!/bin/zsh
set -euo pipefail
setopt null_glob

# GSVA Coherence Test - Two Node
#
# Runs gsva_coh_test on a 2-node QEMU setup.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-gsva-coh}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
RUN_SECS="${RUN_SECS:-120}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_coh_${RANDOM}}"

GSVA_TEST_MODE="${GSVA_TEST_MODE:-all}"

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.gsva_coh.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.gsva_coh.${RUN_ID}.pid"

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

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

start_node() {
  local node_name="$1"
  local role="$2"
  local node_idx="$3"
  local guest_log="$4"
  local qemu_log="$5"
  local pid_file="$6"
  local qemu_extra=()

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  env \
    UB_FM_NODE_ID="$node_name" \
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
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo gsva_coh_test linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} gsva_test_mode=${GSVA_TEST_MODE} ${APPEND_EXTRA}" \
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
  if ! grep -Eq 'GSVA_COH.*object created|GSVA_COH.*I->S|GSVA_COH.*I->M|GSVA_COH.*S->M|GSVA_COH.*M->S' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
    echo "[gsva_coh] FAIL: missing GSVA_COH state transition evidence in QEMU logs" >&2
    return 1
  fi
}

echo "[gsva_coh] run_id=$RUN_ID mode=$GSVA_TEST_MODE"
echo "[gsva_coh] starting nodeA and nodeB..."

start_node nodeA nodeA 0 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB nodeB 1 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "[gsva_coh] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[gsva_coh] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[gsva_coh] FM links ready"

echo "[gsva_coh] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -q 'verdict=PASS' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -q 'verdict=PASS' "$NODEB_GUEST_LOG"; then
    cleanup
    sleep 0.5
    validate_coh_logs
    echo "[gsva_coh] PASS: both nodes completed"
    exit 0
  fi
  if grep -qE 'verdict=FAIL|Kernel panic - not syncing|Call trace:' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[gsva_coh] FAIL: guest reported failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[gsva_coh] FAIL: timeout waiting for completion" >&2
exit 1
