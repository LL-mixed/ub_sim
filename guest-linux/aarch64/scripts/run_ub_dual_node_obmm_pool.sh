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
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-obmm-pool}"
RUN_SECS="${RUN_SECS:-180}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmm_pool_${RANDOM}}"
OBMM_POOL_EXPORT_SIZE_MB="${OBMM_POOL_EXPORT_SIZE_MB:-512}"
OBMM_IMPORT_CACHE_MODE="${OBMM_IMPORT_CACHE_MODE:-auto}"
OBMM_POOL_STRESS_ITERS="${OBMM_POOL_STRESS_ITERS:-20}"
OBMM_POOL_ALL_IPS="${OBMM_POOL_ALL_IPS:-10.0.0.1,10.0.0.2}"

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.obmm_pool.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.obmm_pool.${RUN_ID}.pid"

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

node_ip() {
  case "$1" in
    nodeA) echo "10.0.0.1" ;;
    nodeB) echo "10.0.0.2" ;;
    *) return 1 ;;
  esac
}

start_node() {
  local node_name="$1"
  local role="$2"
  local node_idx="$3"
  local guest_log="$4"
  local qemu_log="$5"
  local pid_file="$6"
  local qemu_extra=()
  local local_ip

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  local_ip="$(node_ip "$node_name")"

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
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_obmm_pool=1 obmm_pool_local_ip=${local_ip} obmm_pool_all_ips=${OBMM_POOL_ALL_IPS} obmm_pool_node_count=2 obmm_pool_export_size_mb=${OBMM_POOL_EXPORT_SIZE_MB} obmm_pool_import_cache_mode=${OBMM_IMPORT_CACHE_MODE} obmm_pool_stress_iters=${OBMM_POOL_STRESS_ITERS} linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} ${APPEND_EXTRA}" \
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

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! grep -qE "$pattern" "$file"; then
    echo "[obmm-pool] FAIL: missing $label in $file" >&2
    return 1
  fi
}

assert_log_absent() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if grep -qE "$pattern" "$file"; then
    echo "[obmm-pool] FAIL: unexpected $label in $file" >&2
    return 1
  fi
}

validate_node_log() {
  local node_name="$1"
  local log_file="$2"
  local owner_slot

  case "$node_name" in
    nodeA) owner_slot=1 ;;
    nodeB) owner_slot=2 ;;
    *) return 1 ;;
  esac

  assert_log_has "$log_file" "\\[run_app\\] run linqu_ub_obmm_pool" "$node_name binary" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] export -> ok mem_id=[0-9]+ uba=0x[0-9a-f]+ token=[0-9]+" "$node_name export" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] metadata exchange -> ok count=2" "$node_name metadata" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] import_all -> ok remote_slots=1" "$node_name import" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool ready -> ok nodes=2" "$node_name pool" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round owner=${owner_slot} write_local -> ok slot=${owner_slot}" "$node_name local write" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=1 -> ok slot=1" "$node_name owner1 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=2 -> ok slot=2" "$node_name owner2 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool rounds -> ok count=2" "$node_name rounds" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] stress done iters=${OBMM_POOL_STRESS_ITERS} remote_slots=1" "$node_name stress" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pass" "$node_name pass" || return 1
  assert_log_absent "$log_file" "\\[ub_obmm_pool\\] fail" "$node_name fail" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "$node_name panic" || return 1
  assert_log_absent "$log_file" "Call trace:" "$node_name call trace" || return 1
}

echo "[obmm-pool] run_id=$RUN_ID export_size_mb=$OBMM_POOL_EXPORT_SIZE_MB cache_mode=$OBMM_IMPORT_CACHE_MODE stress_iters=$OBMM_POOL_STRESS_ITERS"
echo "[obmm-pool] starting nodeA and nodeB..."

start_node nodeA nodeA 0 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB nodeB 1 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "[obmm-pool] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[obmm-pool] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[obmm-pool] FM links ready"

echo "[obmm-pool] waiting for app completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[ub_obmm_pool\] pass' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[ub_obmm_pool\] pass' "$NODEB_GUEST_LOG"; then
    validate_node_log nodeA "$NODEA_GUEST_LOG"
    validate_node_log nodeB "$NODEB_GUEST_LOG"
    echo "[obmm-pool] PASS: both nodes completed"
    echo "[obmm-pool] nodeA:"
    grep '\[ub_obmm_pool\]' "$NODEA_GUEST_LOG" | tail -8
    echo "[obmm-pool] nodeB:"
    grep '\[ub_obmm_pool\]' "$NODEB_GUEST_LOG" | tail -8
    exit 0
  fi
  if grep -qE '\[ub_obmm_pool\] fail|\[run_app\] linqu_ub_obmm_pool failed|Kernel panic - not syncing|Call trace:' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[obmm-pool] FAIL: app or kernel reported failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[obmm-pool] FAIL: timeout waiting for completion" >&2
exit 1
