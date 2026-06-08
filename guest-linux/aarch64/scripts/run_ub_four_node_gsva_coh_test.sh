#!/bin/zsh
set -euo pipefail
setopt null_glob

# GSVA Coherence Test - Four Node
#
# Runs gsva_coh_test on a 4-node QEMU setup.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh_one_entity.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_one_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-1}"
PORT_NUM="${UB_SIM_PORT_NUM:-3}"
SHARED_DIR="${UB_FM_SHARED_DIR:-$ROOT_DIR/out/gsva_coh4_links_${RANDOM}}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-60}"
RUN_SECS="${RUN_SECS:-240}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_coh4_${RANDOM}}"

GSVA_TEST_MODE="${GSVA_TEST_MODE:-all}"
GSVA_MODE="${GSVA_MODE:-legacy_sim_dec}"
GSVA_STRICT="${GSVA_STRICT:-0}"
GSVA_COH_HOLD_PENDING="${GSVA_COH_HOLD_PENDING:-0}"
GSVA_COH_TIMEOUT_MS="${GSVA_COH_TIMEOUT_MS:-5000}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

mkdir -p "$LOG_DIR/${RUN_ID}"

declare -A GUEST_LOGS QEMU_LOGS PID_FILES
for n in A B C D; do
  GUEST_LOGS[$n]="$LOG_DIR/${RUN_ID}/node${n}_guest.log"
  QEMU_LOGS[$n]="$LOG_DIR/${RUN_ID}/node${n}_qemu.log"
  PID_FILES[$n]="$OUT_DIR/ub_node${n}.gsva_coh.${RUN_ID}.pid"
done

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

cleanup() {
  for n in A B C D; do
    local pf="${PID_FILES[$n]}"
    if [[ -f "$pf" ]]; then
      local pid
      pid="$(cat "$pf" 2>/dev/null || true)"
      if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        sleep 0.5
        kill -9 "$pid" 2>/dev/null || true
      fi
      rm -f "$pf"
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
    GSVA_MODE="$GSVA_MODE" \
    GSVA_STRICT="$GSVA_STRICT" \
    GSVA_COH_HOLD_PENDING="$GSVA_COH_HOLD_PENDING" \
    GSVA_COH_TIMEOUT_MS="$GSVA_COH_TIMEOUT_MS" \
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
      -append "console=ttyAMA0 rdinit=/bin/run_demo gsva_coh_test linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} linqu_node_count=4 gsva_test_mode=${GSVA_TEST_MODE} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

wait_all_links_ready() {
  local timeout_s="${1:-60}"
  local deadline=$((SECONDS + timeout_s))
  local nodes=(A B C D)
  while (( SECONDS < deadline )); do
    local all_ready=true
    for n in "${nodes[@]}"; do
      local ready=false
      local ql="${QEMU_LOGS[$n]}"
      local node_name="node${n}"
      # Check status file first (like 2-node script does)
      local sf="$SHARED_DIR/${node_name}_ubcdev0__0.status"
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

validate_coh_logs() {
  local found=false
  local qemu_log_args=()
  local guest_log_args=()

  for n in A B C D; do
    qemu_log_args+=("${QEMU_LOGS[$n]}")
    guest_log_args+=("${GUEST_LOGS[$n]}")
    if grep -Eq 'GSVA_COH' "${QEMU_LOGS[$n]}" 2>/dev/null; then
      found=true
      break
    fi
  done
  if [[ "$found" == "false" ]]; then
    echo "[gsva_coh] FAIL: no GSVA_COH evidence in any QEMU log" >&2
    return 1
  fi
  if [[ "$GSVA_TEST_MODE" == "coh_recovery" ]]; then
    if ! grep -q 'GSVA_COH: WriteAcquire S->M pending inv' "${qemu_log_args[@]}"; then
      echo "[gsva_coh4] FAIL: coh_recovery lacks pending invalidation evidence" >&2
      return 1
    fi
    if ! grep -q 'GSVA_COH: InvAck recovery grant M' "${qemu_log_args[@]}"; then
      echo "[gsva_coh4] FAIL: coh_recovery did not grant M after InvAck" >&2
      return 1
    fi
    if ! grep -q 'coh_recovery Retry error=0' "${guest_log_args[@]}"; then
      echo "[gsva_coh4] FAIL: guest did not observe successful retry after InvAck" >&2
      return 1
    fi
    if ! grep -q 'coh_recovery Query recovered state=3 error=0' "${guest_log_args[@]}"; then
      echo "[gsva_coh4] FAIL: guest query did not observe recovered M state" >&2
      return 1
    fi
    if [[ "$GSVA_MODE" == "arm_mmu" ]]; then
      if ! grep -q 'GSVA_TLB: lookup' "${qemu_log_args[@]}"; then
        echo "[gsva_coh4] FAIL: ARM MMU coh_recovery lacks GSVA_TLB lookup evidence" >&2
        return 1
      fi
      if ! grep -Eq 'GSVA_TLB: flush reason=coh_inv_ack.*cleared=[1-9][0-9]*' "${qemu_log_args[@]}"; then
        echo "[gsva_coh4] FAIL: ARM MMU coh_recovery did not clear installed GSVA TLB metadata" >&2
        return 1
      fi
      if grep -q 'GVA_TCG_TRANSLATE' "${qemu_log_args[@]}"; then
        echo "[gsva_coh4] FAIL: ARM MMU coh_recovery fell back to GVA_TCG_TRANSLATE" >&2
        return 1
      fi
    fi
  fi
}

echo "[gsva_coh4] run_id=$RUN_ID mode=$GSVA_TEST_MODE"
echo "[gsva_coh4] starting 4 nodes..."

start_node nodeA nodeA 0 "${GUEST_LOGS[A]}" "${QEMU_LOGS[A]}" "${PID_FILES[A]}"
start_node nodeB nodeB 1 "${GUEST_LOGS[B]}" "${QEMU_LOGS[B]}" "${PID_FILES[B]}"
start_node nodeC nodeC 2 "${GUEST_LOGS[C]}" "${QEMU_LOGS[C]}" "${PID_FILES[C]}"
start_node nodeD nodeD 3 "${GUEST_LOGS[D]}" "${QEMU_LOGS[D]}" "${PID_FILES[D]}"

echo "[gsva_coh4] waiting for FM links..."
if ! wait_all_links_ready "$LINK_WAIT_SECS"; then
  echo "[gsva_coh4] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[gsva_coh4] FM links ready"

echo "[gsva_coh4] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  local all_pass=true
  for n in A B C D; do
    if ! [[ -f "${GUEST_LOGS[$n]}" ]] || ! grep -q 'verdict=PASS' "${GUEST_LOGS[$n]}"; then
      all_pass=false
      break
    fi
  done
  if [[ "$all_pass" == "true" ]]; then
    cleanup
    sleep 0.5
    validate_coh_logs
    echo "[gsva_coh4] PASS: all nodes completed"
    exit 0
  fi
  for n in A B C D; do
    if grep -qE 'verdict=FAIL|Kernel panic - not syncing|Call trace:' "${GUEST_LOGS[$n]}" 2>/dev/null; then
      echo "[gsva_coh4] FAIL: node${n} reported failure" >&2
      exit 1
    fi
  done
  sleep 0.5
done

echo "[gsva_coh4] FAIL: timeout waiting for completion" >&2
exit 1
