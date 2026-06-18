#!/bin/zsh
set -euo pipefail
setopt null_glob

# NPU GSVA Test - Four Node
#
# Runs npu_gsva_test on a 4-node QEMU setup.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-4}"
PORT_NUM="${UB_SIM_PORT_NUM:-3}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-npu-gsva-test4}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-90}"
RUN_SECS="${RUN_SECS:-180}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_npu_gsva_test_4_${RANDOM}}"

APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

source "$SCRIPT_DIR/qemu_ub_common.sh"
source "$SCRIPT_DIR/ub_gsva_trace_assert.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$LOG_DIR/${RUN_ID}"
NODES=(A B C D)
declare -A GUEST_LOGS QEMU_LOGS PID_FILES
for node_suffix in "${NODES[@]}"; do
  GUEST_LOGS[$node_suffix]="$LOG_DIR/${RUN_ID}/node${node_suffix}_guest.log"
  QEMU_LOGS[$node_suffix]="$LOG_DIR/${RUN_ID}/node${node_suffix}_qemu.log"
  PID_FILES[$node_suffix]="$OUT_DIR/ub_node${node_suffix}.npu_gsva.${RUN_ID}.pid"
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
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_npu_gsva_test=1 linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} linqu_node_count=4 ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

wait_all_links_ready() {
  local timeout_s="${1:-90}"
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

validate_npu_gsva_logs() {
  local node_suffix
  local rc=0
  local node_idx=0

  for node_suffix in "${NODES[@]}"; do
    validate_ub_gsva_trace_logs "[npu_gsva_test_4]" npu "node${node_suffix}" \
      "${QEMU_LOGS[$node_suffix]}" "${GUEST_LOGS[$node_suffix]}" || rc=1
    validate_ub_gsva_peer_matrix "[npu_gsva_test_4]" "node${node_suffix}" \
      "${GUEST_LOGS[$node_suffix]}" "$node_idx" "${#NODES[@]}" || rc=1
    node_idx=$((node_idx + 1))
  done
  return $rc
}

echo "[npu_gsva_test_4] run_id=$RUN_ID"
echo "[npu_gsva_test_4] starting 4 nodes..."

idx=0
for node_suffix in "${NODES[@]}"; do
  start_node "node${node_suffix}" "node${node_suffix}" "$idx" \
    "${GUEST_LOGS[$node_suffix]}" "${QEMU_LOGS[$node_suffix]}" \
    "${PID_FILES[$node_suffix]}"
  idx=$((idx + 1))
done

echo "[npu_gsva_test_4] waiting for FM links..."
if ! wait_all_links_ready "$LINK_WAIT_SECS"; then
  echo "[npu_gsva_test_4] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[npu_gsva_test_4] FM links ready"

echo "[npu_gsva_test_4] waiting for test completion (timeout ${RUN_SECS}s)..."
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
    validate_npu_gsva_logs
    echo "[npu_gsva_test_4] PASS: all nodes completed"
    exit 0
  fi
  for node_suffix in "${NODES[@]}"; do
    local guest_log="${GUEST_LOGS[$node_suffix]}"
    if grep -qE 'verdict=FAIL|Kernel panic - not syncing|Call trace:' "$guest_log" 2>/dev/null; then
      echo "[npu_gsva_test_4] FAIL: node${node_suffix} reported failure" >&2
      exit 1
    fi
  done
  sleep 0.5
done

echo "[npu_gsva_test_4] FAIL: timeout waiting for completion" >&2
exit 1
