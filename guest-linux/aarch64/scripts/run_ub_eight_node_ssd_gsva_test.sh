#!/bin/zsh
set -euo pipefail
setopt null_glob

# SSD GSVA Test - Eight Node
#
# Runs ssd_gsva_test on a 8-node QEMU setup.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-8}"
PORT_NUM="${UB_SIM_PORT_NUM:-7}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-ssd-gsva-test8}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-120}"
RUN_SECS="${RUN_SECS:-240}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_ssd_gsva_test_8_${RANDOM}}"

APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$LOG_DIR/${RUN_ID}"
NODES=(A B C D E F G H)
declare -A GUEST_LOGS QEMU_LOGS PID_FILES
for node_suffix in "${NODES[@]}"; do
  GUEST_LOGS[$node_suffix]="$LOG_DIR/${RUN_ID}/node${node_suffix}_guest.log"
  QEMU_LOGS[$node_suffix]="$LOG_DIR/${RUN_ID}/node${node_suffix}_qemu.log"
  PID_FILES[$node_suffix]="$OUT_DIR/ub_node${node_suffix}.ssd_gsva.${RUN_ID}.pid"
done

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

cleanup() {
  local node_suffix
  for node_suffix in "${NODES[@]}"; do
    local pid_file="${PID_FILES[$node_suffix]}"
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
    UB_SIM_PORT_NUM="$PORT_NUM" \
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
      -append "console=ttyAMA0 rdinit=/bin/run_demo ssd_gsva_test linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} linqu_node_count=8 ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

wait_all_links_ready() {
  local timeout_s="${1:-120}"
  local deadline=$((SECONDS + timeout_s))
  local node_suffix
  while (( SECONDS < deadline )); do
    local all_ready=true
    for node_suffix in "${NODES[@]}"; do
      local node_name="node${node_suffix}"
      local ready=false
      local ql="${QEMU_LOGS[$node_suffix]}"
      local sf="$SHARED_DIR/${node_name}_ubcdev0__1.status"
      if [[ -f "$sf" ]] && grep -q "^state=READY" "$sf"; then
        ready=true
      fi
      if [[ "$ready" == "false" ]] && [[ -f "$ql" ]] && \
         grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$ql"; then
        ready=true
      fi
      if [[ "$ready" == "false" ]]; then
        all_ready=false
        break
      fi
    done
    if [[ "$all_ready" == "true" ]]; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

validate_ssd_gsva_logs() {
  local node_suffix
  for node_suffix in "${NODES[@]}"; do
    if ! grep -q 'UB_SSD: created' "${QEMU_LOGS[$node_suffix]}"; then
      echo "[ssd_gsva_test_8] FAIL: UB_SSD not created on node${node_suffix}" >&2
      return 1
    fi
  done
  return 0
}

echo "[ssd_gsva_test_8] run_id=$RUN_ID"
echo "[ssd_gsva_test_8] starting 8 nodes..."

idx=0
for node_suffix in "${NODES[@]}"; do
  start_node "node${node_suffix}" "node${node_suffix}" "$idx" \
    "${GUEST_LOGS[$node_suffix]}" "${QEMU_LOGS[$node_suffix]}" \
    "${PID_FILES[$node_suffix]}"
  idx=$((idx + 1))
done

echo "[ssd_gsva_test_8] waiting for FM links..."
if ! wait_all_links_ready "$LINK_WAIT_SECS"; then
  echo "[ssd_gsva_test_8] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[ssd_gsva_test_8] FM links ready"

echo "[ssd_gsva_test_8] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  all_pass=true
  for node_suffix in "${NODES[@]}"; do
    local guest_log="${GUEST_LOGS[$node_suffix]}"
    if ! [[ -f "$guest_log" ]] || ! grep -q 'verdict=PASS' "$guest_log"; then
      all_pass=false
      break
    fi
  done
  if [[ "$all_pass" == "true" ]]; then
    cleanup
    sleep 0.5
    validate_ssd_gsva_logs
    echo "[ssd_gsva_test_8] PASS: all nodes completed"
    exit 0
  fi
  for node_suffix in "${NODES[@]}"; do
    local guest_log="${GUEST_LOGS[$node_suffix]}"
    if grep -qE 'verdict=FAIL|Kernel panic - not syncing|Call trace:' "$guest_log" 2>/dev/null; then
      echo "[ssd_gsva_test_8] FAIL: node${node_suffix} reported failure" >&2
      exit 1
    fi
  done
  sleep 0.5
done

echo "[ssd_gsva_test_8] FAIL: timeout waiting for completion" >&2
exit 1
