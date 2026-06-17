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
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dp-microbench}"
RUN_SECS="${RUN_SECS:-180}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
QMP_MODE="${QMP_MODE:-auto}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_dp_microbench_${RANDOM}}"
DP_MODE="${DP_MODE:-legacy-pa}"
DP_SIZE="${DP_SIZE:-2097152}"
DP_ITERS="${DP_ITERS:-32768}"
DP_CHUNK_SIZE="${DP_CHUNK_SIZE:-64}"
DP_VERIFY="${DP_VERIFY:-0}"
DP_GENERIC_PTE_OFFSET="${DP_GENERIC_PTE_OFFSET:-0x1000}"
DP_GSVA_BASE="${DP_GSVA_BASE:-0x700000000000}"
DP_GSVA_GENERATION="${DP_GSVA_GENERATION:-0x44504d424701}"
DP_APPEND="obmm_dp_mode=${DP_MODE} obmm_dp_size=${DP_SIZE} obmm_dp_iters=${DP_ITERS} obmm_dp_chunk_size=${DP_CHUNK_SIZE}"
DP_APPEND="${DP_APPEND} obmm_dp_generic_pte_offset=${DP_GENERIC_PTE_OFFSET}"
DP_APPEND="${DP_APPEND} obmm_dp_gsva_base=${DP_GSVA_BASE} obmm_dp_gsva_generation=${DP_GSVA_GENERATION}"
if [[ "$DP_VERIFY" == "1" ]]; then
  DP_APPEND="${DP_APPEND} obmm_dp_verify=1"
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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.dp_microbench.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.dp_microbench.${RUN_ID}.pid"
NODEA_QMP="$SHARED_DIR/qmp/nodeA.qmp"
NODEB_QMP="$SHARED_DIR/qmp/nodeB.qmp"
QMP_ACTIVE=0

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
  local qemu_pause=()
  local qmp_flag=()

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  if [[ "$QMP_MODE" == "on" ]]; then
    qmp_flag=(-qmp unix:"$qmp_socket",server=on,wait=off)
    qemu_pause=(-S)
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
      "${qemu_pause[@]}" \
      ${qmp_flag[@]} \
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_obmm_dataplane_microbench=1 linqu_urma_dp_role=${role} ${DP_APPEND} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
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

resume_qmp() {
  local socket_path="$1"

  python3 - "$socket_path" <<'PY'
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
}

validate_dp_logs() {
  case "$DP_MODE" in
    legacy|legacy-pa)
      if grep -qE 'SIM_DEC: GVA_MAP success|GSVA_MAP:|GSVA_TLB:|GSVA_COH:' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: legacy mode produced GVA/GSVA data-path logs" >&2
        return 1
      fi
      if ! grep -Eq 'SIM_DEC_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'SIM_DEC_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: legacy mode completed without nonzero PA cpu read/write stats" >&2
        return 1
      fi
      ;;
    generic|generic-gva|gva)
      if ! grep -q 'SIM_DEC: GVA_MAP success' "$NODEA_QEMU_LOG" ||
         ! grep -q 'SIM_DEC: GVA_MAP success' "$NODEB_QEMU_LOG" ||
         ! grep -q 'GVA_S3_MAP' "$NODEA_QEMU_LOG" ||
         ! grep -q 'GVA_S3_MAP' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: generic GVA mode completed without GVA map evidence" >&2
        return 1
      fi
      if ! grep -Eq 'GVA_PATH gva_path=cpu_window op=read ' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GVA_PATH gva_path=cpu_window op=write ' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GVA_PATH gva_path=cpu_window op=read ' "$NODEB_QEMU_LOG" ||
         ! grep -Eq 'GVA_PATH gva_path=cpu_window op=write ' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: generic GVA mode completed without cpu-window read/write evidence" >&2
        return 1
      fi
      if ! grep -Eq 'GVA_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GVA_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG" ||
         ! grep -Eq 'SIM_DEC_STATS .*gva_cpu_reads=[1-9][0-9]* .*gva_cpu_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'SIM_DEC_STATS .*gva_cpu_reads=[1-9][0-9]* .*gva_cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: generic GVA mode completed without nonzero read/write stats" >&2
        return 1
      fi
      ;;
    gsva)
      if ! grep -Eq 'GSVA_MAP: map_id=[0-9]+ .*source=2 profile=1' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GSVA_MAP: map_id=[0-9]+ .*source=2 profile=1' "$NODEB_QEMU_LOG" ||
         ! grep -Eq 'GSVA_MAP: cpu_window registered at pa=.*size=[0-9a-f]+' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GSVA_MAP: cpu_window registered at pa=.*size=[0-9a-f]+' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: GSVA mode completed without GSVA map/cpu-window evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_TLB: lookup' "$NODEA_QEMU_LOG" ||
         ! grep -q 'GSVA_TLB: lookup' "$NODEB_QEMU_LOG" ||
         ! grep -q 'GSVA_COH:' "$NODEA_QEMU_LOG" ||
         ! grep -q 'GSVA_COH:' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: GSVA mode completed without TLB/coherence evidence" >&2
        return 1
      fi
      if ! grep -Eq 'SIM_DEC_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'SIM_DEC_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG" ||
         ! grep -Eq 'GVA_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
         ! grep -Eq 'GVA_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
        echo "[dp_microbench] FAIL: GSVA mode completed without nonzero remote read/write stats" >&2
        return 1
      fi
      ;;
    *)
      echo "[dp_microbench] FAIL: unknown DP_MODE=$DP_MODE" >&2
      return 1
      ;;
  esac
}

echo "[dp_microbench] run_id=$RUN_ID mode=$DP_MODE size=$DP_SIZE iters=$DP_ITERS chunk=$DP_CHUNK_SIZE verify=$DP_VERIFY"
echo "[dp_microbench] starting nodeA and nodeB..."

start_node nodeA nodeA "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE" "$NODEA_QMP"
start_node nodeB nodeB "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE" "$NODEB_QMP"

if [[ "$QMP_MODE" == "on" ]]; then
  if ! wait_for_qmp_socket "$NODEA_QMP" 10 || ! wait_for_qmp_socket "$NODEB_QMP" 10; then
    echo "[dp_microbench] FAIL: QMP socket not ready" >&2
    exit 1
  fi
  QMP_ACTIVE=1
fi

if [[ "$QMP_MODE" == "none" || "$QMP_MODE" == "auto" ]]; then
  QMP_ACTIVE=0
fi

if [[ "$QMP_ACTIVE" == "1" ]]; then
  resume_qmp "$NODEA_QMP"
  resume_qmp "$NODEB_QMP"
else
  echo "[dp_microbench] running without QMP control path"
fi

echo "[dp_microbench] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[dp_microbench] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[dp_microbench] FM links ready"

echo "[dp_microbench] waiting for completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[obmm_dataplane_microbench\] result=done' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[obmm_dataplane_microbench\] result=done' "$NODEB_GUEST_LOG"; then
    cleanup
    sleep 0.5
    if ! validate_dp_logs; then
      exit 1
    fi
    echo "[dp_microbench] PASS: both nodes completed"
    echo "[dp_microbench] nodeA stats:"
    grep '\[obmm_dataplane_microbench\]' "$NODEA_GUEST_LOG" | tail -6
    echo "[dp_microbench] nodeB stats:"
    grep '\[obmm_dataplane_microbench\]' "$NODEB_GUEST_LOG" | tail -6
    exit 0
  fi
  if grep -qE '\[obmm_dataplane_microbench\] bench failed|\[obmm_dataplane_microbench\] .*import failed|\[obmm_dataplane_microbench\] .*export failed|\[obmm_dataplane_microbench\] MAP_GSVA failed|\[run_(app|demo)\] action failed|Kernel panic - not syncing' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[dp_microbench] FAIL: guest reported benchmark/import/export/action failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[dp_microbench] FAIL: timeout waiting for completion" >&2
exit 1
