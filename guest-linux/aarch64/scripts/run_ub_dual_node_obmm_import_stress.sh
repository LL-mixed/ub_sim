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
QMP_MODE="${QMP_MODE:-auto}"
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
STRESS_GVA_MODE="${STRESS_GVA_MODE:-legacy}"
STRESS_GVA_MAP_SOURCE="${STRESS_GVA_MAP_SOURCE:-legacy}"
STRESS_GVA_ADDRESS_PROFILE="${STRESS_GVA_ADDRESS_PROFILE:-generic}"
STRESS_GVA_CACHE_POLICY="${STRESS_GVA_CACHE_POLICY:-wt}"
STRESS_GVA_VMID="${STRESS_GVA_VMID:-0}"
STRESS_GVA_ASID="${STRESS_GVA_ASID:-0}"
STRESS_GVA_TID="${STRESS_GVA_TID:-0}"
STRESS_GVA_P_TAG="${STRESS_GVA_P_TAG:-0}"
STRESS_GVA_ACCESS_FLAGS="${STRESS_GVA_ACCESS_FLAGS:-0}"
STRESS_GVA_TOKEN_VALUE="${STRESS_GVA_TOKEN_VALUE:-0}"
STRESS_GVA_ID="${STRESS_GVA_ID:-0}"
STRESS_GVA_USER_VA="${STRESS_GVA_USER_VA:-}"
STRESS_GVA_HOME_VA="${STRESS_GVA_HOME_VA:-}"
STRESS_GVA_PTE_OFFSET="${STRESS_GVA_PTE_OFFSET:-}"
STRESS_GSVA_BASE="${STRESS_GSVA_BASE:-0x700000000000}"
STRESS_GSVA_GENERATION="${STRESS_GSVA_GENERATION:-0x535456410101}"
STRESS_DIRECTORY_MESI_ACCEPTANCE="${STRESS_DIRECTORY_MESI_ACCEPTANCE:-0}"
STRESS_REQUIRE_COHERENCE_LOGS="${STRESS_REQUIRE_COHERENCE_LOGS:-$STRESS_DIRECTORY_MESI_ACCEPTANCE}"
if [[ "$STRESS_DIRECTORY_MESI_ACCEPTANCE" == "1" ]]; then
  STRESS_SIZE=2097152
  STRESS_PATTERN=seq
  STRESS_ITERS=64
  STRESS_FLUSH=periodic
  STRESS_PERIOD=8
  STRESS_CHUNK_SIZE=64
  STRESS_VERIFY=1
  STRESS_GVA_MODE=generic
  STRESS_GVA_MAP_SOURCE=gva
  STRESS_GVA_CACHE_POLICY=directory-mesi
  if [[ "$APPEND_EXTRA" != *"obmm.skip_cache_maintain="* ]]; then
    APPEND_EXTRA="${APPEND_EXTRA} obmm.skip_cache_maintain=1"
  fi
fi
STRESS_APPEND="obmm_stress_size=${STRESS_SIZE} obmm_stress_pattern=${STRESS_PATTERN} obmm_stress_iters=${STRESS_ITERS} obmm_stress_flush=${STRESS_FLUSH} obmm_stress_period=${STRESS_PERIOD} obmm_stress_chunk_size=${STRESS_CHUNK_SIZE} obmm_stress_seed=${STRESS_SEED}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_mode=${STRESS_GVA_MODE}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_map_source=${STRESS_GVA_MAP_SOURCE}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_address_profile=${STRESS_GVA_ADDRESS_PROFILE}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_cache_policy=${STRESS_GVA_CACHE_POLICY}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_vmid=${STRESS_GVA_VMID}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_asid=${STRESS_GVA_ASID}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_tid=${STRESS_GVA_TID}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_p_tag=${STRESS_GVA_P_TAG}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_access_flags=${STRESS_GVA_ACCESS_FLAGS}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_token_value=${STRESS_GVA_TOKEN_VALUE}"
STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_id=${STRESS_GVA_ID}"
if [[ -n "$STRESS_GVA_USER_VA" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_user_va=${STRESS_GVA_USER_VA}"
fi
if [[ -n "$STRESS_GVA_HOME_VA" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_home_va=${STRESS_GVA_HOME_VA}"
fi
if [[ -n "$STRESS_GVA_PTE_OFFSET" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_gva_pte_offset=${STRESS_GVA_PTE_OFFSET}"
fi
if [[ "$STRESS_GVA_MODE" == "gsva" ]]; then
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_gsva_base=${STRESS_GSVA_BASE}"
  STRESS_APPEND="${STRESS_APPEND} obmm_stress_gsva_generation=${STRESS_GSVA_GENERATION}"
fi
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
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_obmm_import_stress=1 linqu_urma_dp_role=${role} ${STRESS_APPEND} ${APPEND_EXTRA}" \
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

validate_gva_logs() {
  if [[ "$STRESS_GVA_MODE" == "legacy" ]]; then
    return 0
  fi

  if [[ "$STRESS_GVA_MODE" == "gsva" ]]; then
    if ! grep -Eq 'GSVA_MAP: map_id=[0-9]+ .*source=2 profile=1' "$NODEA_QEMU_LOG" ||
       ! grep -Eq 'GSVA_MAP: map_id=[0-9]+ .*source=2 profile=1' "$NODEB_QEMU_LOG" ||
       ! grep -Eq 'GSVA_MAP: cpu_window registered at pa=.*size=[0-9a-f]+' "$NODEA_QEMU_LOG" ||
       ! grep -Eq 'GSVA_MAP: cpu_window registered at pa=.*size=[0-9a-f]+' "$NODEB_QEMU_LOG"; then
      echo "[stress] FAIL: GSVA mode completed without GSVA_MAP/cpu-window evidence" >&2
      return 1
    fi

    if ! grep -q 'GSVA_TLB: lookup' "$NODEA_QEMU_LOG" ||
       ! grep -q 'GSVA_TLB: lookup' "$NODEB_QEMU_LOG" ||
       ! grep -q 'GSVA_COH:' "$NODEA_QEMU_LOG" ||
       ! grep -q 'GSVA_COH:' "$NODEB_QEMU_LOG"; then
      echo "[stress] FAIL: GSVA mode completed without TLB/coherence data-path evidence" >&2
      return 1
    fi

    if ! grep -Eq 'SIM_DEC_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
       ! grep -Eq 'SIM_DEC_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG" ||
       ! grep -Eq 'GVA_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
       ! grep -Eq 'GVA_STATS .*remote_reads=[1-9][0-9]* .*remote_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
      echo "[stress] FAIL: GSVA mode completed without nonzero remote read/write stats" >&2
      return 1
    fi
    return 0
  fi

  if ! grep -q 'SIM_DEC: GVA_MAP success' "$NODEA_QEMU_LOG" ||
     ! grep -q 'SIM_DEC: GVA_MAP success' "$NODEB_QEMU_LOG" ||
     ! grep -q 'GVA_S3_MAP' "$NODEA_QEMU_LOG" ||
     ! grep -q 'GVA_S3_MAP' "$NODEB_QEMU_LOG" ||
     ! grep -q 'GVA_ROUTE_DUMP state=active' "$NODEA_QEMU_LOG" ||
     ! grep -q 'GVA_ROUTE_DUMP state=active' "$NODEB_QEMU_LOG"; then
    echo "[stress] FAIL: GVA mode completed without QEMU GVA_MAP evidence" >&2
    return 1
  fi

  if ! grep -Eq 'GVA_PATH gva_path=cpu_window op=read ' "$NODEA_QEMU_LOG" ||
     ! grep -Eq 'GVA_PATH gva_path=cpu_window op=write ' "$NODEA_QEMU_LOG" ||
     ! grep -Eq 'GVA_PATH gva_path=cpu_window op=read ' "$NODEB_QEMU_LOG" ||
     ! grep -Eq 'GVA_PATH gva_path=cpu_window op=write ' "$NODEB_QEMU_LOG"; then
    echo "[stress] FAIL: GVA mode completed without cpu-window path read/write evidence" >&2
    return 1
  fi

  if ! grep -Eq 'GVA_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
     ! grep -Eq 'GVA_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
    echo "[stress] FAIL: GVA mode completed without nonzero GVA_STATS read/write evidence" >&2
    return 1
  fi
  if ! grep -Eq 'SIM_DEC_STATS .*gva_cpu_reads=[1-9][0-9]* .*gva_cpu_writes=[1-9][0-9]* .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEA_QEMU_LOG" ||
     ! grep -Eq 'SIM_DEC_STATS .*gva_cpu_reads=[1-9][0-9]* .*gva_cpu_writes=[1-9][0-9]* .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
    echo "[stress] FAIL: GVA mode completed without nonzero SIM_DEC_STATS read/write evidence" >&2
    return 1
  fi

  if [[ "$STRESS_GVA_CACHE_POLICY" == "directory-mesi" ||
        "$STRESS_GVA_CACHE_POLICY" == "mesi" ]]; then
    if ! grep -Eq 'GVA_S3_MAP .*cache_policy=4' "$NODEA_QEMU_LOG" ||
       ! grep -Eq 'GVA_S3_MAP .*cache_policy=4' "$NODEB_QEMU_LOG"; then
      echo "[stress] FAIL: directory-MESI completed without cache_policy=4 GVA map evidence" >&2
      return 1
    fi
  fi

  if [[ "$STRESS_REQUIRE_COHERENCE_LOGS" == "1" ]]; then
    if ! grep -Eq 'OBMM_COH_GET[SM]|OBMM_COH_WB|OBMM_COH_INV' "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG"; then
      echo "[stress] FAIL: directory-MESI completed without OBMM_COH message evidence" >&2
      return 1
    fi
  fi
}

echo "[stress] run_id=$RUN_ID size=$STRESS_SIZE pattern=$STRESS_PATTERN iters=$STRESS_ITERS flush=$STRESS_FLUSH chunk=$STRESS_CHUNK_SIZE"
echo "[stress] starting nodeA and nodeB..."

start_node nodeA nodeA "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE" "$NODEA_QMP"
start_node nodeB nodeB "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE" "$NODEB_QMP"

if [[ "$QMP_MODE" == "on" ]]; then
  if ! wait_for_qmp_socket "$NODEA_QMP" 10 || ! wait_for_qmp_socket "$NODEB_QMP" 10; then
    echo "[stress] FAIL: QMP socket not ready" >&2
    exit 1
  else
    QMP_ACTIVE=1
  fi
fi

if [[ "$QMP_MODE" == "none" || "$QMP_MODE" == "auto" ]]; then
  QMP_ACTIVE=0
fi

if [[ "$QMP_ACTIVE" == "1" ]]; then
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
else
  echo "[stress] running without QMP control path"
fi

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
    cleanup
    sleep 0.5
    if ! validate_gva_logs; then
      exit 1
    fi
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
  if grep -qE '\[obmm_import_stress\] import failed|\[obmm_import_stress\] export failed|\[run_app\] action failed|Kernel panic - not syncing' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[stress] FAIL: guest reported import/export/action failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[stress] FAIL: timeout waiting for completion" >&2
exit 1
