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
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-obmm-gsva}"
RUN_SECS="${RUN_SECS:-120}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmm_gsva_${RANDOM}}"
OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-identity}"
OBMM_GSVA_BASE="${OBMM_GSVA_BASE:-0x700000000000}"
OBMM_GSVA_SIZE="${OBMM_GSVA_SIZE:-0x400000}"
OBMM_GSVA_NODE_COUNT="${OBMM_GSVA_NODE_COUNT:-2}"
GSVA_MODE="${GSVA_MODE:-arm_mmu}"
GSVA_STRICT="${GSVA_STRICT:-1}"
LOG_PREFIX="[obmm-gsva]"

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.obmm_gsva.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.obmm_gsva.${RUN_ID}.pid"

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

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  env \
    GSVA_MODE="$GSVA_MODE" \
    GSVA_STRICT="$GSVA_STRICT" \
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
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_obmm_gsva=1 linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} obmm_gsva_mode=${OBMM_GSVA_MODE} obmm_gsva_base=${OBMM_GSVA_BASE} obmm_gsva_size=${OBMM_GSVA_SIZE} obmm_gsva_node_count=${OBMM_GSVA_NODE_COUNT} ${APPEND_EXTRA}" \
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

validate_identity_logs() {
  if ! grep -q '\[obmm_gsva\] kernel aperture registry -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] kernel aperture registry -> ok' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: missing kernel aperture registry evidence" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] fixed export -> ok' "$NODEA_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: home did not create fixed-UBA export" >&2
    return 1
  fi
  if ! grep -q 'OBMM: GSVA fixed UBA mapped' "$NODEA_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: kernel did not map fixed UBA on home node" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] result=done mode=identity role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=identity role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: identity mode did not complete on both roles" >&2
    return 1
  fi
  if ! grep -Eq "ptr=${OBMM_GSVA_BASE}|ptr=$(printf '%#x' "$((OBMM_GSVA_BASE))")" "$NODEA_GUEST_LOG" ||
     ! grep -Eq "ptr=${OBMM_GSVA_BASE}|ptr=$(printf '%#x' "$((OBMM_GSVA_BASE))")" "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: result logs do not preserve the expected GSVA pointer" >&2
    return 1
  fi
  if ! grep -Eq "GSVA_MAP: map_id=[0-9]+ .*home_va=0x$(printf '%x' "$((OBMM_GSVA_BASE))") .*source=2 profile=1" "$NODEB_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: peer QEMU log lacks GSVA V1 MAP evidence" >&2
    return 1
  fi
  if ! grep -Eq "GSVA_MAP: cpu_window registered at pa=.*size=$(printf '%x' "$((OBMM_GSVA_SIZE))")" "$NODEB_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: peer QEMU log lacks GSVA cpu_window registration evidence" >&2
    return 1
  fi
  if [[ "$GSVA_MODE" == "arm_mmu" ]]; then
    if ! grep -q 'GSVA_TLB: lookup' "$NODEB_QEMU_LOG"; then
      echo "$LOG_PREFIX FAIL: ARM MMU mode lacks GSVA_TLB lookup evidence" >&2
      return 1
    fi
    if ! grep -q 'GSVA_COH:' "$NODEB_QEMU_LOG"; then
      echo "$LOG_PREFIX FAIL: ARM MMU mode lacks GSVA_COH evidence" >&2
      return 1
    fi
    if grep -q 'GVA_TCG_TRANSLATE' "$NODEB_QEMU_LOG"; then
      echo "$LOG_PREFIX FAIL: ARM MMU mode unexpectedly used SIM_GVA_TCG data path" >&2
      return 1
    fi
  fi
}

validate_conflict_logs() {
  if ! grep -q '\[obmm_gsva\] result=done mode=conflict role=home reason=normal-obmm-mmap-rejected' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=conflict role=peer reason=normal-obmm-mmap-rejected' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: conflict mode did not reject normal OBMM mmap on both nodes" >&2
    return 1
  fi
  if ! grep -qE 'overlaps active GSVA aperture|File exists' "$NODEA_GUEST_LOG" ||
     ! grep -qE 'overlaps active GSVA aperture|File exists' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: conflict mode lacks aperture conflict evidence" >&2
    return 1
  fi
}

validate_stale_generation_logs() {
  if ! grep -q '\[obmm_gsva\] result=done mode=stale-generation role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=stale-generation role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: stale-generation mode did not complete on both nodes" >&2
    return 1
  fi
  if ! grep -q 'stale_generation=' "$NODEA_GUEST_LOG" ||
     ! grep -q 'stale_generation=' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: stale-generation mode lacks stale generation evidence" >&2
    return 1
  fi
}

validate_kernel_aperture_proc_logs() {
  if ! grep -q '\[obmm_gsva\] kernel aperture proc -> 1 ' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] kernel aperture proc -> 1 ' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: missing /proc/obmm/gsva_aperture active evidence" >&2
    return 1
  fi
}

validate_invalid_offset_logs() {
  if ! grep -q '\[obmm_gsva\] result=done mode=invalid-offset role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=invalid-offset role=peer bad_pte_offset=0x1000' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: invalid-offset mode did not reject GSVA pte_offset on both roles" >&2
    return 1
  fi
  if grep -Eq "GSVA_MAP: .*profile=1" "$NODEB_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: invalid-offset mode unexpectedly programmed a GSVA route" >&2
    return 1
  fi
  if ! grep -Eq 'GSVA identity mapping requires local_va/home_va/remote_uba be equal and pte_offset 0|GSVA identity import outside active aperture or not identity' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: invalid-offset mode lacks guest kernel rejection evidence" >&2
    return 1
  fi
}

validate_matrix_logs() {
  if ! grep -q '\[obmm_gsva\] kernel aperture registry -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] kernel aperture registry -> ok' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: matrix mode lacks kernel aperture registry evidence" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] result=done mode=matrix node=0 node_count=2' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=matrix node=1 node_count=2' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: matrix mode did not complete on both nodes" >&2
    return 1
  fi
  if ! grep -q 'value_from_last=0x4753564d00000100' "$NODEA_GUEST_LOG" ||
     ! grep -q 'value_from_node0=0x4753564d00000001' "$NODEB_GUEST_LOG" ||
     ! grep -q 'value_from_last=0x4753564d00000101' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: matrix mode lacks distinct cross-node write evidence" >&2
    return 1
  fi
  if ! grep -Eq "GSVA_MAP: map_id=[0-9]+ .*home_va=0x$(printf '%x' "$((OBMM_GSVA_BASE + OBMM_GSVA_SIZE))") .*source=2 profile=1" "$NODEA_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: matrix mode lacks nodeA import route for nodeB slice" >&2
    return 1
  fi
  if ! grep -Eq "GSVA_MAP: map_id=[0-9]+ .*home_va=0x$(printf '%x' "$((OBMM_GSVA_BASE))") .*source=2 profile=1" "$NODEB_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: matrix mode lacks nodeB import route for nodeA slice" >&2
    return 1
  fi
}

validate_mmap_mode_logs() {
  if ! grep -q '\[obmm_gsva\] mmap-mode MAP_GSVA segment -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] mmap-mode MAP_GSVA segment -> ok' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: mmap-mode lacks MAP_GSVA segment mmap success evidence" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] mmap-mode MAP_GSVA non-gsva reject -> ok' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] mmap-mode MAP_GSVA non-gsva reject -> ok' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: mmap-mode lacks MAP_GSVA non-GSVA rejection evidence" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] result=done mode=mmap-mode role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=mmap-mode role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: mmap-mode did not complete on both nodes" >&2
    return 1
  fi
  if ! grep -q 'normal_reject_errno=17' "$NODEA_GUEST_LOG" ||
     ! grep -q 'normal_reject_errno=17' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: mmap-mode lacks normal OBMM aperture rejection evidence" >&2
    return 1
  fi
}

validate_anonymous_collision_logs() {
  if ! grep -q '\[obmm_gsva\] anonymous mmap rejected by kernel gsva reserve -> ok errno=17' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] anonymous mmap rejected by kernel gsva reserve -> ok errno=17' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: anonymous-collision lacks kernel mmap reserve rejection evidence" >&2
    return 1
  fi
  if ! grep -q '\[obmm_gsva\] result=done mode=anonymous-collision role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=anonymous-collision role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: anonymous-collision did not complete on both nodes" >&2
    return 1
  fi
}

validate_outside_aperture_logs() {
  if ! grep -q '\[obmm_gsva\] result=done mode=outside-aperture role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=outside-aperture role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: outside-aperture mode did not reject fixed UBA on both nodes" >&2
    return 1
  fi
  if ! grep -q 'GSVA fixed UBA export outside active aperture' "$NODEA_GUEST_LOG" ||
     ! grep -q 'GSVA fixed UBA export outside active aperture' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: outside-aperture mode lacks OBMM rejection evidence" >&2
    return 1
  fi
}

validate_outside_import_logs() {
  if ! grep -q '\[obmm_gsva\] result=done mode=outside-import role=home' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[obmm_gsva\] result=done mode=outside-import role=peer' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: outside-import mode did not reject GSVA import on both roles" >&2
    return 1
  fi
  if ! grep -q 'GSVA identity import outside active aperture or not identity' "$NODEB_GUEST_LOG"; then
    echo "$LOG_PREFIX FAIL: outside-import mode lacks OBMM import rejection evidence" >&2
    return 1
  fi
  if grep -Eq "GSVA_MAP: .*profile=1" "$NODEB_QEMU_LOG"; then
    echo "$LOG_PREFIX FAIL: outside-import mode unexpectedly programmed a GSVA route" >&2
    return 1
  fi
}

echo "$LOG_PREFIX run_id=$RUN_ID mode=$OBMM_GSVA_MODE base=$OBMM_GSVA_BASE size=$OBMM_GSVA_SIZE node_count=$OBMM_GSVA_NODE_COUNT"
echo "$LOG_PREFIX starting nodeA and nodeB..."

start_node nodeA nodeA 0 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB nodeB 1 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "$LOG_PREFIX waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "$LOG_PREFIX FAIL: FM links not ready" >&2
  exit 1
fi
echo "$LOG_PREFIX FM links ready"

echo "$LOG_PREFIX waiting for app completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[obmm_gsva\] result=done' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[obmm_gsva\] result=done' "$NODEB_GUEST_LOG"; then
    sleep 1
    validate_kernel_aperture_proc_logs
    case "$OBMM_GSVA_MODE" in
      identity)
        validate_identity_logs
        ;;
      conflict)
        validate_conflict_logs
        ;;
      stale-generation)
        validate_stale_generation_logs
        ;;
      invalid-offset)
        validate_invalid_offset_logs
        ;;
      matrix)
        validate_matrix_logs
        ;;
      mmap-mode)
        validate_mmap_mode_logs
        ;;
      anonymous-collision)
        validate_anonymous_collision_logs
        ;;
      outside-aperture)
        validate_outside_aperture_logs
        ;;
      outside-import)
        validate_outside_import_logs
        ;;
    esac
    echo "$LOG_PREFIX PASS: both nodes completed"
    echo "$LOG_PREFIX nodeA:"
    grep '\[obmm_gsva\]' "$NODEA_GUEST_LOG" | tail -8
    echo "$LOG_PREFIX nodeB:"
    grep '\[obmm_gsva\]' "$NODEB_GUEST_LOG" | tail -8
    exit 0
  fi
  if grep -qE '\[obmm_gsva\] result=fail|\[run_app\] linqu_ub_obmm_gsva failed' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "$LOG_PREFIX FAIL: app reported failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "$LOG_PREFIX FAIL: timeout waiting for completion" >&2
exit 1
