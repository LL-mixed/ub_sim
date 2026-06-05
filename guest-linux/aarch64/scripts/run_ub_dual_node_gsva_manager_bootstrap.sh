#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-gsva-manager}"
RUN_SECS="${RUN_SECS:-120}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_mgr_${RANDOM}}"
GVA_MANAGER_GENERATION="${GVA_MANAGER_GENERATION:-0x475356410001}"
GVA_MANAGER_APERTURE_BASE="${GVA_MANAGER_APERTURE_BASE:-0x700000000000}"
GVA_MANAGER_APERTURE_SIZE="${GVA_MANAGER_APERTURE_SIZE:-0x1000000}"
GVA_MANAGER_CONFLICT_NODE="${GVA_MANAGER_CONFLICT_NODE:-}"
EXPECT_FAILURE="${EXPECT_FAILURE:-0}"

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.gsva_mgr.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.gsva_mgr.${RUN_ID}.pid"

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

cleanup() {
  local pid_file
  for pid_file in "$NODEA_PID_FILE" "$NODEB_PID_FILE"; do
    if [[ -f "$pid_file" ]]; then
      local pid=$(cat "$pid_file" 2>/dev/null || true)
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
  local conflict_append=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi
  if [[ "$GVA_MANAGER_CONFLICT_NODE" == "$node_idx" ]]; then
    conflict_append="gva_manager_conflict=1"
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
      -append "console=ttyAMA0 rdinit=/bin/run_demo gva_manager_bootstrap linqu_urma_dp_role=${role} gva_manager_node_id=${node_idx} gva_manager_node_count=2 gva_manager_generation=${GVA_MANAGER_GENERATION} gva_manager_aperture_base=${GVA_MANAGER_APERTURE_BASE} gva_manager_aperture_size=${GVA_MANAGER_APERTURE_SIZE} ${conflict_append} ${APPEND_EXTRA}" \
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
    if [[ -f "$nodea_status" ]]; then
      local state=$(grep "^state=" "$nodea_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then nodea_ready=true; fi
    fi
    if [[ "$nodea_ready" == "false" ]] && [[ -f "$nodea_log" ]] && \
       grep -qE "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodea_log"; then
      nodea_ready=true
    fi
    if [[ -f "$nodeb_status" ]]; then
      local state=$(grep "^state=" "$nodeb_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then nodeb_ready=true; fi
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

extract_aperture() {
  local log_file="$1"
  grep '\[gva_manager\] result=done' "$log_file" | tail -1 | \
    sed -n 's/.*aperture_base=\([^ ]*\).*aperture_size=\([^ ]*\).*/\1 \2/p'
}

validate_manager_logs() {
  local a_aperture
  local b_aperture

  if ! grep -q '\[gva_manager\] manager queues -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[gva_manager\] manager queues -> ok' "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: missing OBMM MPMC queue evidence" >&2
    return 1
  fi
  if ! grep -q '\[gva_manager\] aperture reserved registry=process-local' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[gva_manager\] aperture reserved registry=process-local' "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: missing aperture reservation evidence" >&2
    return 1
  fi
  if ! grep -q '\[gva_manager\] kernel aperture registry -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[gva_manager\] kernel aperture registry -> ok' "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: missing kernel/OBMM aperture registry evidence" >&2
    return 1
  fi
  if ! grep -q 'registry=kernel-obmm' "$NODEA_GUEST_LOG" ||
     ! grep -q 'registry=kernel-obmm' "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: managers did not complete with kernel/OBMM registry" >&2
    return 1
  fi

  a_aperture="$(extract_aperture "$NODEA_GUEST_LOG")"
  b_aperture="$(extract_aperture "$NODEB_GUEST_LOG")"
  if [[ -z "$a_aperture" || -z "$b_aperture" || "$a_aperture" != "$b_aperture" ]]; then
    echo "[gsva-manager] FAIL: managers did not agree on aperture" >&2
    echo "[gsva-manager] nodeA=$a_aperture nodeB=$b_aperture" >&2
    return 1
  fi
}

validate_expected_failure() {
  if [[ -z "$GVA_MANAGER_CONFLICT_NODE" ]]; then
    echo "[gsva-manager] FAIL: EXPECT_FAILURE=1 requires GVA_MANAGER_CONFLICT_NODE" >&2
    return 1
  fi
  if ! grep -q '\[gva_manager\] result=fail' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: expected manager failure was not observed" >&2
    return 1
  fi
  if ! grep -q '\[gva_manager\] aperture reserve failed' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: failure was not an aperture reservation conflict" >&2
    return 1
  fi
}

echo "[gsva-manager] run_id=$RUN_ID generation=$GVA_MANAGER_GENERATION aperture_base=$GVA_MANAGER_APERTURE_BASE aperture_size=$GVA_MANAGER_APERTURE_SIZE"
echo "[gsva-manager] starting nodeA and nodeB..."

start_node nodeA nodeA 0 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB nodeB 1 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "[gsva-manager] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[gsva-manager] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[gsva-manager] FM links ready"

echo "[gsva-manager] waiting for manager completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[gva_manager\] result=done' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[gva_manager\] result=done' "$NODEB_GUEST_LOG"; then
    validate_manager_logs
    echo "[gsva-manager] PASS: both managers completed"
    echo "[gsva-manager] nodeA:"
    grep '\[gva_manager\]' "$NODEA_GUEST_LOG" | tail -8
    echo "[gsva-manager] nodeB:"
    grep '\[gva_manager\]' "$NODEB_GUEST_LOG" | tail -8
    exit 0
  fi
  if grep -qE '\[gva_manager\] result=fail' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    if [[ "$EXPECT_FAILURE" == "1" ]]; then
      validate_expected_failure
      echo "[gsva-manager] PASS: expected manager failure observed"
      exit 0
    fi
    echo "[gsva-manager] FAIL: manager reported failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[gsva-manager] FAIL: timeout waiting for completion" >&2
exit 1
