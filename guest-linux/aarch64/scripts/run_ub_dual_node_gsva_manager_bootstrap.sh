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
GVA_MANAGER_ALLOCATE_SEGMENT="${GVA_MANAGER_ALLOCATE_SEGMENT:-0}"
GVA_MANAGER_RETIRE_SEGMENT="${GVA_MANAGER_RETIRE_SEGMENT:-0}"
GVA_MANAGER_REUSE_SEGMENT="${GVA_MANAGER_REUSE_SEGMENT:-0}"
GVA_MANAGER_IMPORT_SEGMENT="${GVA_MANAGER_IMPORT_SEGMENT:-0}"
GVA_MANAGER_ROTATE_TOKEN="${GVA_MANAGER_ROTATE_TOKEN:-0}"
GVA_MANAGER_SEGMENT_SIZE="${GVA_MANAGER_SEGMENT_SIZE:-0x400000}"
GVA_MANAGER_SEGMENT_ALIGNMENT="${GVA_MANAGER_SEGMENT_ALIGNMENT:-0x1000}"
GVA_MANAGER_HOME_NODE="${GVA_MANAGER_HOME_NODE:-0}"
GVA_MANAGER_CACHE_POLICY="${GVA_MANAGER_CACHE_POLICY:-wt}"
GVA_MANAGER_ACCESS_FLAGS="${GVA_MANAGER_ACCESS_FLAGS:-0}"
GVA_MANAGER_CONFLICT_NODE="${GVA_MANAGER_CONFLICT_NODE:-}"
EXPECT_FAILURE="${EXPECT_FAILURE:-0}"
GSVA_MODE="${GSVA_MODE:-legacy_sim_dec}"
GSVA_STRICT="${GSVA_STRICT:-0}"

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
  local segment_append=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi
  if [[ "$GVA_MANAGER_CONFLICT_NODE" == "$node_idx" ]]; then
    conflict_append="gva_manager_conflict=1"
  fi
  if [[ "$GVA_MANAGER_ALLOCATE_SEGMENT" == "1" || "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" || "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
    segment_append="gva_manager_allocate_segment=1 gva_manager_segment_size=${GVA_MANAGER_SEGMENT_SIZE} gva_manager_segment_alignment=${GVA_MANAGER_SEGMENT_ALIGNMENT} gva_manager_home_node=${GVA_MANAGER_HOME_NODE} gva_manager_cache_policy=${GVA_MANAGER_CACHE_POLICY} gva_manager_access_flags=${GVA_MANAGER_ACCESS_FLAGS}"
    if [[ "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
      segment_append="${segment_append} gva_manager_import_segment=1"
    fi
    if [[ "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
      segment_append="${segment_append} gva_manager_rotate_token=1"
    fi
    if [[ "$GVA_MANAGER_RETIRE_SEGMENT" == "1" ]]; then
      segment_append="${segment_append} gva_manager_retire_segment=1"
    fi
    if [[ "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
      segment_append="${segment_append} gva_manager_reuse_segment=1"
    fi
  fi

  env \
    UB_FM_NODE_ID="$node_name" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    GSVA_MODE="$GSVA_MODE" \
    GSVA_STRICT="$GSVA_STRICT" \
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
      -append "console=ttyAMA0 rdinit=/bin/run_demo gva_manager_bootstrap linqu_urma_dp_role=${role} gva_manager_node_id=${node_idx} gva_manager_node_count=2 gva_manager_generation=${GVA_MANAGER_GENERATION} gva_manager_aperture_base=${GVA_MANAGER_APERTURE_BASE} gva_manager_aperture_size=${GVA_MANAGER_APERTURE_SIZE} ${segment_append} ${conflict_append} ${APPEND_EXTRA}" \
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
  local a_segment
  local b_segment
  local a_initial_segment
  local b_initial_segment
  local a_retired
  local b_retired
  local a_reused
  local b_reused

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
  if ! grep -q '\[gva_manager\] kernel aperture proc -> 1 ' "$NODEA_GUEST_LOG" ||
     ! grep -q '\[gva_manager\] kernel aperture proc -> 1 ' "$NODEB_GUEST_LOG"; then
    echo "[gsva-manager] FAIL: missing /proc/obmm/gsva_aperture active evidence" >&2
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
  if [[ "$GVA_MANAGER_ALLOCATE_SEGMENT" == "1" || "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" || "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
    if ! grep -q '\[gva_manager\] segment active' "$NODEA_GUEST_LOG" ||
       ! grep -q '\[gva_manager\] segment active' "$NODEB_GUEST_LOG"; then
      echo "[gsva-manager] FAIL: missing active segment evidence" >&2
      return 1
    fi
    if ! grep -q 'descriptor=kernel' "$NODEA_GUEST_LOG" ||
       ! grep -q 'descriptor=kernel' "$NODEB_GUEST_LOG" ||
       ! grep -q 'gsva descriptor action=manager-alloc' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG"; then
      echo "[gsva-manager] FAIL: missing kernel descriptor distribution evidence" >&2
      return 1
    fi
    a_segment="$(grep '\[gva_manager\] segment active' "$NODEA_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
    b_segment="$(grep '\[gva_manager\] segment active' "$NODEB_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
    a_initial_segment="$(grep '\[gva_manager\] segment active' "$NODEA_GUEST_LOG" | head -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
    b_initial_segment="$(grep '\[gva_manager\] segment active' "$NODEB_GUEST_LOG" | head -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
    if [[ -z "$a_segment" || -z "$b_segment" || "$a_segment" != "$b_segment" ]]; then
      echo "[gsva-manager] FAIL: managers did not agree on active segment" >&2
      echo "[gsva-manager] nodeA=$a_segment nodeB=$b_segment" >&2
      return 1
    fi
    if [[ "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
      local imported_segment
      imported_segment="$(grep '\[gva_manager\] manager descriptor import segment_id=' "$NODEB_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*home_va=\([^ ]*\).*epoch=\([^ ]*\).*p_tag=\([^ ]*\).*token_id=\([^ ]*\).*/\1 \2 \3 \4 \5/p')"
      if [[ -z "$imported_segment" ]]; then
        echo "[gsva-manager] FAIL: missing peer descriptor import evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_MAP: map_id=.*segment_id=0xc' "$NODEB_QEMU_LOG"; then
        echo "[gsva-manager] FAIL: missing QEMU GSVA_MAP descriptor import evidence" >&2
        return 1
      fi
    fi
    if [[ "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
      if ! grep -q '\[gva_manager\] manager token rotation committed' "$NODEA_GUEST_LOG"; then
        echo "[gsva-manager] FAIL: missing home token rotation commit evidence" >&2
        return 1
      fi
      if ! grep -q '\[gva_manager\] manager token revoke holder ack' "$NODEB_GUEST_LOG"; then
        echo "[gsva-manager] FAIL: missing peer token revoke holder ACK evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_ROUTE: token revoke pending' "$NODEB_QEMU_LOG" ||
         ! grep -q 'GSVA_ROUTE: token revoke ack' "$NODEB_QEMU_LOG" ||
         ! grep -q 'GSVA_COH: ReadAcquire' "$NODEB_QEMU_LOG"; then
        echo "[gsva-manager] FAIL: missing QEMU token revoke/ACK/read evidence" >&2
        return 1
      fi
      local a_token
      local b_token
      a_token="$(grep '\[gva_manager\] manager token rotation committed' "$NODEA_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*token_id=\([^ ]*\).*old_token_value=\([^ ]*\).*new_token_value=\([^ ]*\).*/\1 \2 \3 \4/p')"
      b_token="$(grep '\[gva_manager\] manager token revoke holder ack' "$NODEB_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*token_id=\([^ ]*\).*old_token_value=\([^ ]*\).*new_token_value=\([^ ]*\).*/\1 \2 \3 \4/p')"
      if [[ -z "$a_token" || -z "$b_token" || "$a_token" != "$b_token" ]]; then
        echo "[gsva-manager] FAIL: managers did not agree on token rotation" >&2
        echo "[gsva-manager] nodeA=$a_token nodeB=$b_token" >&2
        return 1
      fi
      if [[ "$(echo "$a_token" | awk '{print $3}')" == "$(echo "$a_token" | awk '{print $4}')" ]]; then
        echo "[gsva-manager] FAIL: token rotation did not change token value" >&2
        return 1
      fi
      if [[ "$GSVA_MODE" == "arm_mmu" ]]; then
        if ! grep -q 'GSVA_TLB: flush reason=token_revoke_pending' "$NODEB_QEMU_LOG" ||
           ! grep -q 'GSVA_TLB: flush reason=token_revoke_ack' "$NODEB_QEMU_LOG"; then
          echo "[gsva-manager] FAIL: ARM MMU token revoke did not flush peer GSVA TLB metadata" >&2
          return 1
        fi
      fi
    fi
    if [[ "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
      if ! grep -q '\[gva_manager\] segment retired' "$NODEA_GUEST_LOG" ||
         ! grep -q '\[gva_manager\] segment retired' "$NODEB_GUEST_LOG"; then
        echo "[gsva-manager] FAIL: missing retired segment evidence" >&2
        return 1
      fi
      if ! grep -q 'OBMM: GSVA segment retired:' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG"; then
        echo "[gsva-manager] FAIL: missing kernel descriptor retire evidence" >&2
        return 1
      fi
      a_retired="$(grep '\[gva_manager\] segment retired' "$NODEA_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
      b_retired="$(grep '\[gva_manager\] segment retired' "$NODEB_GUEST_LOG" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
      if [[ -z "$a_retired" || -z "$b_retired" || "$a_retired" != "$b_retired" || "$a_retired" != "$a_initial_segment" || "$b_retired" != "$b_initial_segment" ]]; then
        echo "[gsva-manager] FAIL: managers did not agree on retired segment" >&2
        echo "[gsva-manager] nodeA=$a_retired nodeB=$b_retired initialA=$a_initial_segment initialB=$b_initial_segment" >&2
        return 1
      fi
      if [[ "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_ROTATE_TOKEN" == "1" ]]; then
        if ! grep -q '\[gva_manager\] manager retire holder route retired' "$NODEB_GUEST_LOG"; then
          echo "[gsva-manager] FAIL: missing peer manager RetireAck route-retired evidence" >&2
          return 1
        fi
        if ! grep -q 'GSVA_RETIRE: segment_id=' "$NODEB_QEMU_LOG" ||
           ! grep -q 'GSVA_UNMAP: map_id=.*tombstone=yes' "$NODEB_QEMU_LOG"; then
          echo "[gsva-manager] FAIL: missing QEMU GSVA retire/tombstone evidence before manager ACK cleanup" >&2
          return 1
        fi
      fi
    fi
    if [[ "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
      if (( $(grep -c '\[gva_manager\] segment active' "$NODEA_GUEST_LOG") < 2 )) ||
         (( $(grep -c '\[gva_manager\] segment active' "$NODEB_GUEST_LOG") < 2 )); then
        echo "[gsva-manager] FAIL: reuse did not create a second active segment" >&2
        return 1
      fi
      if ! grep -q '\[gva_manager\] segment reused' "$NODEA_GUEST_LOG" ||
         ! grep -q '\[gva_manager\] segment reused' "$NODEB_GUEST_LOG"; then
        echo "[gsva-manager] FAIL: missing reused segment evidence" >&2
        return 1
      fi
      a_reused="$(grep '\[gva_manager\] segment reused' "$NODEA_GUEST_LOG" | tail -1 | sed -n 's/.*old_segment_id=\([^ ]*\).*new_segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3 \4/p')"
      b_reused="$(grep '\[gva_manager\] segment reused' "$NODEB_GUEST_LOG" | tail -1 | sed -n 's/.*old_segment_id=\([^ ]*\).*new_segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3 \4/p')"
      if [[ -z "$a_reused" || -z "$b_reused" || "$a_reused" != "$b_reused" ]]; then
        echo "[gsva-manager] FAIL: managers did not agree on reused segment" >&2
        echo "[gsva-manager] nodeA=$a_reused nodeB=$b_reused" >&2
        return 1
      fi
      if [[ "$(echo "$a_reused" | awk '{print $1}')" == "$(echo "$a_reused" | awk '{print $2}')" ]]; then
        echo "[gsva-manager] FAIL: reused segment id did not change" >&2
        return 1
      fi
    fi
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

echo "[gsva-manager] run_id=$RUN_ID generation=$GVA_MANAGER_GENERATION aperture_base=$GVA_MANAGER_APERTURE_BASE aperture_size=$GVA_MANAGER_APERTURE_SIZE allocate_segment=$GVA_MANAGER_ALLOCATE_SEGMENT retire_segment=$GVA_MANAGER_RETIRE_SEGMENT reuse_segment=$GVA_MANAGER_REUSE_SEGMENT rotate_token=$GVA_MANAGER_ROTATE_TOKEN"
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
