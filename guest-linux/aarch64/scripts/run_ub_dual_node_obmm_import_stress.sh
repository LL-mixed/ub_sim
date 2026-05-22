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
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-stress}"
RUN_SECS="${RUN_SECS:-180}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_stress_${RANDOM}}"
MAIN_PID=$$
STRESS_SIZE="${STRESS_SIZE:-4194304}"
STRESS_PATTERN="${STRESS_PATTERN:-seq}"
STRESS_ITERS="${STRESS_ITERS:-1000}"
STRESS_FLUSH="${STRESS_FLUSH:-periodic}"
STRESS_PERIOD="${STRESS_PERIOD:-100}"
STRESS_CHUNK_SIZE="${STRESS_CHUNK_SIZE:-8}"
STRESS_SEED="${STRESS_SEED:-1}"
STRESS_VERIFY="${STRESS_VERIFY:-0}"
STRESS_READ_ONLY="${STRESS_READ_ONLY:-0}"
STRESS_WRITE_ONLY="${STRESS_WRITE_ONLY:-0}"
STRESS_APPEND="obmm_stress_size=${STRESS_SIZE} obmm_stress_pattern=${STRESS_PATTERN} obmm_stress_iters=${STRESS_ITERS} obmm_stress_flush=${STRESS_FLUSH} obmm_stress_period=${STRESS_PERIOD} obmm_stress_chunk_size=${STRESS_CHUNK_SIZE} obmm_stress_seed=${STRESS_SEED}"
if [[ "$STRESS_VERIFY" == "1" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_verify=1"
fi
if [[ "$STRESS_READ_ONLY" == "1" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_read_only=1"
fi
if [[ "$STRESS_WRITE_ONLY" == "1" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_write_only=1"
fi

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.stress.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.stress.${RUN_ID}.pid"
NODEA_QMP="$SHARED_DIR/qmp/nodeA.qmp"
NODEB_QMP="$SHARED_DIR/qmp/nodeB.qmp"

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR/qmp"

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
  local node_id="$1"
  local role="$2"
  local guest_log="$3"
  local qemu_log="$4"
  local pid_file="$5"
  local qmp_socket="$6"
  local qemu_extra=()

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    "$QEMU_BIN" \
      -S \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -m 8G \
      -nodefaults \
      -nographic \
      -qmp unix:"$qmp_socket",server=on,wait=off \
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo obmm_import_stress linqu_urma_dp_role=${role} ${STRESS_APPEND} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
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

wait_for_qmp_socket() {
  local socket_path="$1"
  local timeout_s="${2:-10}"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    if [[ -S "$socket_path" ]]; then
      return 0
    fi
    sleep 0.1
  done
  return 1
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

echo "[stress] run_id=$RUN_ID size=$STRESS_SIZE pattern=$STRESS_PATTERN iters=$STRESS_ITERS flush=$STRESS_FLUSH chunk=$STRESS_CHUNK_SIZE"
echo "[stress] starting nodeA and nodeB..."

start_node nodeA nodeA "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE" "$NODEA_QMP"
start_node nodeB nodeB "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE" "$NODEB_QMP"

if ! wait_for_qmp_socket "$NODEA_QMP" 10 || ! wait_for_qmp_socket "$NODEB_QMP" 10; then
  echo "[stress] FAIL: QMP socket not ready" >&2
  exit 1
fi

# Resume QEMU via QMP
python3 - "$NODEA_QMP" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\r\n')
s.recv(4096)
s.sendall(b'{"execute":"cont"}\r\n')
s.recv(4096)
s.close()
PY

python3 - "$NODEB_QMP" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(sys.argv[1])
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\r\n')
s.recv(4096)
s.sendall(b'{"execute":"cont"}\r\n')
s.recv(4096)
s.close()
PY

echo "[stress] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[stress] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[stress] FM links ready"

echo "[stress] waiting for stress completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[obmm_import_stress\] result=done' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[obmm_import_stress\] result=done' "$NODEB_GUEST_LOG"; then
    echo "[stress] PASS: both nodes completed"
    echo "[stress] nodeA stats:"
    grep '\[obmm_import_stress\]' "$NODEA_GUEST_LOG" | tail -5
    echo "[stress] nodeB stats:"
    grep '\[obmm_import_stress\]' "$NODEB_GUEST_LOG" | tail -5
    exit 0
  fi
  if grep -qE '\[obmm_import_stress\] stress_run failed' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[stress] FAIL: stress_run failed" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[stress] FAIL: timeout waiting for completion" >&2
exit 1
