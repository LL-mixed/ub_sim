#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
GVA_MANAGER_NODE_COUNT="${GVA_MANAGER_NODE_COUNT:-4}"
case "$GVA_MANAGER_NODE_COUNT" in
  4)
    DEFAULT_TOPOLOGY_FILE="$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh.ini"
    DEFAULT_GENERATION="0x475356410004"
    DEFAULT_APERTURE_SIZE="0x1000000"
    NODE_NAMES=(nodeA nodeB nodeC nodeD)
    ;;
  8)
    DEFAULT_TOPOLOGY_FILE="$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini"
    DEFAULT_GENERATION="0x475356410008"
    DEFAULT_APERTURE_SIZE="0x4000000"
    NODE_NAMES=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
    ;;
  *)
    echo "[gsva-manager] FAIL: GVA_MANAGER_NODE_COUNT must be 4 or 8" >&2
    exit 1
    ;;
esac
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$DEFAULT_TOPOLOGY_FILE}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
PORT_NUM="${UB_SIM_PORT_NUM:-$((GVA_MANAGER_NODE_COUNT - 1))}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-gsva-manager${GVA_MANAGER_NODE_COUNT}}"
QMP_DIR="$SHARED_DIR/qmp"
RUN_SECS="${RUN_SECS:-180}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_mgr${GVA_MANAGER_NODE_COUNT}_${RANDOM}}"
GVA_MANAGER_GENERATION="${GVA_MANAGER_GENERATION:-$DEFAULT_GENERATION}"
GVA_MANAGER_APERTURE_BASE="${GVA_MANAGER_APERTURE_BASE:-0x700000000000}"
GVA_MANAGER_APERTURE_SIZE="${GVA_MANAGER_APERTURE_SIZE:-$DEFAULT_APERTURE_SIZE}"
GVA_MANAGER_ALLOCATE_SEGMENT="${GVA_MANAGER_ALLOCATE_SEGMENT:-0}"
GVA_MANAGER_IMPORT_SEGMENT="${GVA_MANAGER_IMPORT_SEGMENT:-0}"
GVA_MANAGER_RETIRE_SEGMENT="${GVA_MANAGER_RETIRE_SEGMENT:-0}"
GVA_MANAGER_REUSE_SEGMENT="${GVA_MANAGER_REUSE_SEGMENT:-0}"
GVA_MANAGER_COH_RECOVERY="${GVA_MANAGER_COH_RECOVERY:-0}"
GVA_MANAGER_SEGMENT_SIZE="${GVA_MANAGER_SEGMENT_SIZE:-0x400000}"
GVA_MANAGER_SEGMENT_ALIGNMENT="${GVA_MANAGER_SEGMENT_ALIGNMENT:-0x1000}"
GVA_MANAGER_HOME_NODE="${GVA_MANAGER_HOME_NODE:-0}"
GVA_MANAGER_CACHE_POLICY="${GVA_MANAGER_CACHE_POLICY:-wt}"
GVA_MANAGER_ACCESS_FLAGS="${GVA_MANAGER_ACCESS_FLAGS:-0}"
GVA_MANAGER_CONFLICT_NODE="${GVA_MANAGER_CONFLICT_NODE:-}"
GSVA_MODE="${GSVA_MODE:-sim_dec}"
GSVA_STRICT="${GSVA_STRICT:-0}"
GSVA_COH_HOLD_PENDING="${GSVA_COH_HOLD_PENDING:-0}"
GSVA_COH_TIMEOUT_MS="${GSVA_COH_TIMEOUT_MS:-0}"
EXPECT_FAILURE="${EXPECT_FAILURE:-0}"
LOG_PREFIX="[gsva-manager${GVA_MANAGER_NODE_COUNT}]"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

mkdir -p "$LOG_DIR/${RUN_ID}"
rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR" "$QMP_DIR"

guest_log_for() {
  echo "$LOG_DIR/${RUN_ID}/$1_guest.log"
}

qemu_log_for() {
  echo "$LOG_DIR/${RUN_ID}/$1_qemu.log"
}

qmp_socket_for() {
  echo "$QMP_DIR/$1.qmp"
}

pid_file_for() {
  echo "$OUT_DIR/ub_$1.gsva_mgr${GVA_MANAGER_NODE_COUNT}.${RUN_ID}.pid"
}

cleanup() {
  local node_name
  local pid_file
  local pid

  for node_name in "${NODE_NAMES[@]}"; do
    pid_file="$(pid_file_for "$node_name")"
    if [[ -f "$pid_file" ]]; then
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
  local node_idx="$2"
  local guest_log="$3"
  local qemu_log="$4"
  local pid_file="$5"
  local node_cna
  local qmp_socket
  local qemu_extra=()
  local conflict_append=""
  local segment_append=""

  node_cna="$(node_cna_for "$node_idx")"
  qmp_socket="$(qmp_socket_for "$node_name")"
  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi
  if [[ "$GVA_MANAGER_CONFLICT_NODE" == "$node_idx" ]]; then
    conflict_append="gva_manager_conflict=1"
  fi
  if [[ "$GVA_MANAGER_ALLOCATE_SEGMENT" == "1" || "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" || "$GVA_MANAGER_COH_RECOVERY" == "1" ]]; then
    segment_append="gva_manager_allocate_segment=1 gva_manager_segment_size=${GVA_MANAGER_SEGMENT_SIZE} gva_manager_segment_alignment=${GVA_MANAGER_SEGMENT_ALIGNMENT} gva_manager_home_node=${GVA_MANAGER_HOME_NODE} gva_manager_cache_policy=${GVA_MANAGER_CACHE_POLICY} gva_manager_access_flags=${GVA_MANAGER_ACCESS_FLAGS}"
    if [[ "$GVA_MANAGER_IMPORT_SEGMENT" == "1" || "$GVA_MANAGER_COH_RECOVERY" == "1" ]]; then
      segment_append="${segment_append} gva_manager_import_segment=1"
    fi
    if [[ "$GVA_MANAGER_RETIRE_SEGMENT" == "1" ]]; then
      segment_append="${segment_append} gva_manager_retire_segment=1"
    fi
    if [[ "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
      segment_append="${segment_append} gva_manager_reuse_segment=1"
    fi
    if [[ "$GVA_MANAGER_COH_RECOVERY" == "1" ]]; then
      segment_append="${segment_append} gva_manager_coh_recovery=1"
    fi
  fi

  env \
    GSVA_MODE="$GSVA_MODE" \
    GSVA_STRICT="$GSVA_STRICT" \
    GSVA_COH_HOLD_PENDING="$GSVA_COH_HOLD_PENDING" \
    GSVA_COH_TIMEOUT_MS="$GSVA_COH_TIMEOUT_MS" \
    UB_FM_NODE_ID="$node_name" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_SIM_PORT_NUM="$PORT_NUM" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    "$QEMU_BIN" \
      -S \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -smp "$QEMU_SMP" \
      -m "$QEMU_MEM" \
      -nodefaults \
      -nographic \
      -qmp unix:"$qmp_socket",server=on,wait=off \
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo gva_manager_bootstrap linqu_urma_dp_role=${node_name} linqu_cna=${node_cna} gva_manager_node_id=${node_idx} gva_manager_node_count=${GVA_MANAGER_NODE_COUNT} gva_manager_generation=${GVA_MANAGER_GENERATION} gva_manager_aperture_base=${GVA_MANAGER_APERTURE_BASE} gva_manager_aperture_size=${GVA_MANAGER_APERTURE_SIZE} ${segment_append} ${conflict_append} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

node_cna_for() {
  local node_idx="$1"
  printf '0x%x' "$((0xc4c2 + node_idx * 0x10))"
}

wait_for_qmp_socket() {
  local node_name="$1"
  local qmp_socket="$2"
  local timeout_s="${3:-30}"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    if [[ -S "$qmp_socket" ]]; then
      return 0
    fi
    sleep 0.1
  done

  echo "$LOG_PREFIX FAIL: QMP socket not ready for $node_name: $qmp_socket" >&2
  return 1
}

wait_for_fm_endpoints() {
  local timeout_s="${1:-30}"
  local deadline=$((SECONDS + timeout_s))
  local expected=$((GVA_MANAGER_NODE_COUNT * (GVA_MANAGER_NODE_COUNT - 1)))
  local endpoint_files=()

  while (( SECONDS < deadline )); do
    endpoint_files=("$SHARED_DIR"/node*_ubcdev0__*.ini(N))
    if (( ${#endpoint_files[@]} >= expected )); then
      return 0
    fi
    sleep 0.1
  done

  endpoint_files=("$SHARED_DIR"/node*_ubcdev0__*.ini(N))
  echo "$LOG_PREFIX FAIL: FM endpoints not ready: have=${#endpoint_files[@]} expected=$expected" >&2
  return 1
}

cont_qemu() {
  local qmp_socket="$1"
  python3 - "$qmp_socket" <<'PY'
import socket
import sys

path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(path)
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\r\n')
s.recv(4096)
s.sendall(b'{"execute":"cont"}\r\n')
s.recv(4096)
s.close()
PY
}

extract_aperture() {
  local log_file="$1"
  grep '\[gva_manager\] result=done' "$log_file" | tail -1 | \
    sed -n 's/.*aperture_base=\([^ ]*\).*aperture_size=\([^ ]*\).*/\1 \2/p'
}

validate_manager_logs() {
  local node_name
  local guest_log
  local aperture=""
  local node_aperture
  local segment=""
  local node_segment
  local node_initial_segment
  local retired=""
  local node_retired
  local reused=""
  local node_reused
  local home_pos=$((GVA_MANAGER_HOME_NODE + 1))
  local home_node_name="${NODE_NAMES[$home_pos]}"
  local qemu_log
  local peer_count_expected=$((GVA_MANAGER_NODE_COUNT - 1))

  for node_name in "${NODE_NAMES[@]}"; do
    guest_log="$(guest_log_for "$node_name")"
    if ! grep -q "\[gva_manager\] obmm bootstrap -> ok count=${GVA_MANAGER_NODE_COUNT}" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks OBMM bootstrap evidence" >&2
      return 1
    fi
    if ! grep -q '\[gva_manager\] manager queues -> ok' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks OBMM MPMC queue evidence" >&2
      return 1
    fi
    if ! grep -q "\[gva_manager\] bootstrap hello -> ok peers=$((GVA_MANAGER_NODE_COUNT - 1))" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name did not observe all peers" >&2
      return 1
    fi
    if ! grep -q '\[gva_manager\] aperture reserved registry=process-local' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks aperture reservation evidence" >&2
      return 1
    fi
    if ! grep -q '\[gva_manager\] kernel aperture registry -> ok' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks kernel/OBMM aperture registry evidence" >&2
      return 1
    fi
    if ! grep -q '\[gva_manager\] kernel aperture proc -> 1 ' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks /proc/obmm/gsva_aperture active evidence" >&2
      return 1
    fi
    if ! grep -q 'registry=kernel-obmm' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name did not complete with kernel/OBMM registry" >&2
      return 1
    fi

    node_aperture="$(extract_aperture "$guest_log")"
    if [[ -z "$node_aperture" ]]; then
      echo "$LOG_PREFIX FAIL: $node_name lacks result aperture" >&2
      return 1
    fi
    if [[ -z "$aperture" ]]; then
      aperture="$node_aperture"
    elif [[ "$node_aperture" != "$aperture" ]]; then
      echo "$LOG_PREFIX FAIL: managers did not agree on aperture" >&2
      echo "$LOG_PREFIX expected=$aperture ${node_name}=$node_aperture" >&2
      return 1
    fi
    if [[ "$GVA_MANAGER_ALLOCATE_SEGMENT" == "1" || "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
      if ! grep -q '\[gva_manager\] segment active' "$guest_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks active segment evidence" >&2
        return 1
      fi
      node_segment="$(grep '\[gva_manager\] segment active' "$guest_log" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
      node_initial_segment="$(grep '\[gva_manager\] segment active' "$guest_log" | head -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
      if [[ -z "$node_segment" ]]; then
        echo "$LOG_PREFIX FAIL: $node_name lacks active segment fields" >&2
        return 1
      fi
      if [[ -z "$segment" ]]; then
        segment="$node_segment"
      elif [[ "$node_segment" != "$segment" ]]; then
        echo "$LOG_PREFIX FAIL: managers did not agree on active segment" >&2
        echo "$LOG_PREFIX expected=$segment ${node_name}=$node_segment" >&2
        return 1
      fi
      if [[ "$GVA_MANAGER_RETIRE_SEGMENT" == "1" || "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
        if ! grep -q '\[gva_manager\] segment retired' "$guest_log"; then
          echo "$LOG_PREFIX FAIL: $node_name lacks retired segment evidence" >&2
          return 1
        fi
        node_retired="$(grep '\[gva_manager\] segment retired' "$guest_log" | tail -1 | sed -n 's/.*segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3/p')"
        if [[ -z "$node_retired" || "$node_retired" != "$node_initial_segment" ]]; then
          echo "$LOG_PREFIX FAIL: $node_name retired segment does not match active segment" >&2
          echo "$LOG_PREFIX initial=$node_initial_segment retired=$node_retired" >&2
          return 1
        fi
        if [[ -z "$retired" ]]; then
          retired="$node_retired"
        elif [[ "$node_retired" != "$retired" ]]; then
          echo "$LOG_PREFIX FAIL: managers did not agree on retired segment" >&2
          echo "$LOG_PREFIX expected=$retired ${node_name}=$node_retired" >&2
          return 1
        fi
      fi
      if [[ "$GVA_MANAGER_REUSE_SEGMENT" == "1" ]]; then
        if (( $(grep -c '\[gva_manager\] segment active' "$guest_log") < 2 )); then
          echo "$LOG_PREFIX FAIL: $node_name reuse did not create a second active segment" >&2
          return 1
        fi
        if ! grep -q '\[gva_manager\] segment reused' "$guest_log"; then
          echo "$LOG_PREFIX FAIL: $node_name lacks reused segment evidence" >&2
          return 1
        fi
        node_reused="$(grep '\[gva_manager\] segment reused' "$guest_log" | tail -1 | sed -n 's/.*old_segment_id=\([^ ]*\).*new_segment_id=\([^ ]*\).*gsva_base=\([^ ]*\).*size=\([^ ]*\).*/\1 \2 \3 \4/p')"
        if [[ -z "$node_reused" ]]; then
          echo "$LOG_PREFIX FAIL: $node_name lacks reused segment fields" >&2
          return 1
        fi
        if [[ -z "$reused" ]]; then
          reused="$node_reused"
        elif [[ "$node_reused" != "$reused" ]]; then
          echo "$LOG_PREFIX FAIL: managers did not agree on reused segment" >&2
          echo "$LOG_PREFIX expected=$reused ${node_name}=$node_reused" >&2
          return 1
        fi
        if [[ "$(echo "$node_reused" | awk '{print $1}')" == "$(echo "$node_reused" | awk '{print $2}')" ]]; then
          echo "$LOG_PREFIX FAIL: reused segment id did not change" >&2
          return 1
        fi
      fi
    fi
  done

  if [[ "$GVA_MANAGER_COH_RECOVERY" == "1" ]]; then
    guest_log="$(guest_log_for "$home_node_name")"
    if ! grep -q "\[gva_manager\] manager coherence recovery committed .*acked_peers=${peer_count_expected}" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $home_node_name lacks manager coherence recovery commit evidence" >&2
      return 1
    fi
    for node_name in "${NODE_NAMES[@]}"; do
      if [[ "$node_name" == "$home_node_name" ]]; then
        continue
      fi
      guest_log="$(guest_log_for "$node_name")"
      qemu_log="$(qemu_log_for "$node_name")"
      if ! grep -q '\[gva_manager\] manager coherence recovery pending' "$guest_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks manager coherence recovery pending evidence" >&2
        return 1
      fi
      if ! grep -q '\[gva_manager\] manager coherence recovery holder ack' "$guest_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks manager holder ACK evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_COH: WriteAcquire S->M pending inv' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks pending invalidation evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_QUERY_COHERENCE: .*pending=1' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks pending query evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_COH: InvAck recovery grant M' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks InvAck recovery evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_TLB: flush reason=coh_inv_ack' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks coh_inv_ack TLB flush evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_QUERY_COHERENCE: .*state=M error=0' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name lacks recovered M query evidence" >&2
        return 1
      fi
      if [[ "$GSVA_MODE" == "arm_mmu" ]] && grep -q 'GVA_TCG_TRANSLATE' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name fell back to GVA_TCG_TRANSLATE" >&2
        return 1
      fi
    done
  fi
}

validate_expected_failure() {
  local node_name
  local any_fail="0"
  local any_reserve_fail="0"

  if [[ -z "$GVA_MANAGER_CONFLICT_NODE" ]]; then
    echo "$LOG_PREFIX FAIL: EXPECT_FAILURE=1 requires GVA_MANAGER_CONFLICT_NODE" >&2
    return 1
  fi

  for node_name in "${NODE_NAMES[@]}"; do
    if grep -q '\[gva_manager\] result=fail' "$(guest_log_for "$node_name")"; then
      any_fail="1"
    fi
    if grep -q '\[gva_manager\] aperture reserve failed' "$(guest_log_for "$node_name")"; then
      any_reserve_fail="1"
    fi
  done

  if [[ "$any_fail" != "1" || "$any_reserve_fail" != "1" ]]; then
    echo "$LOG_PREFIX FAIL: expected aperture reservation conflict was not observed" >&2
    return 1
  fi
}

node_has_failure() {
  local log_file="$1"
  grep -qE '\[gva_manager\] result=fail|\[run_demo\] linqu_gva_manager failed|\[run_demo\] action failed: gva_manager_bootstrap|Kernel panic - not syncing' "$log_file"
}

print_node_summary() {
  local node_name="$1"
  echo "$LOG_PREFIX ${node_name}:"
  grep '\[gva_manager\]' "$(guest_log_for "$node_name")" | tail -8
}

echo "$LOG_PREFIX run_id=$RUN_ID generation=$GVA_MANAGER_GENERATION aperture_base=$GVA_MANAGER_APERTURE_BASE aperture_size=$GVA_MANAGER_APERTURE_SIZE allocate_segment=$GVA_MANAGER_ALLOCATE_SEGMENT import_segment=$GVA_MANAGER_IMPORT_SEGMENT retire_segment=$GVA_MANAGER_RETIRE_SEGMENT reuse_segment=$GVA_MANAGER_REUSE_SEGMENT coh_recovery=$GVA_MANAGER_COH_RECOVERY"
echo "$LOG_PREFIX topology=$TOPOLOGY_FILE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP port_num=$PORT_NUM"
echo "$LOG_PREFIX starting ${GVA_MANAGER_NODE_COUNT} nodes..."

integer idx=0
for node_name in "${NODE_NAMES[@]}"; do
  start_node "$node_name" "$idx" "$(guest_log_for "$node_name")" "$(qemu_log_for "$node_name")" "$(pid_file_for "$node_name")"
  idx=$((idx + 1))
  sleep 0.2
done

echo "$LOG_PREFIX waiting for QMP sockets and FM endpoint files..."
for node_name in "${NODE_NAMES[@]}"; do
  wait_for_qmp_socket "$node_name" "$(qmp_socket_for "$node_name")" 30
done
wait_for_fm_endpoints 30
for node_name in "${NODE_NAMES[@]}"; do
  cont_qemu "$(qmp_socket_for "$node_name")"
done

echo "$LOG_PREFIX waiting for manager completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  done_count=0
  fail_count=0
  for node_name in "${NODE_NAMES[@]}"; do
    if [[ -f "$(guest_log_for "$node_name")" ]] &&
       grep -qE '\[gva_manager\] result=done' "$(guest_log_for "$node_name")"; then
      done_count=$((done_count + 1))
    fi
    if [[ -f "$(guest_log_for "$node_name")" ]] &&
       node_has_failure "$(guest_log_for "$node_name")"; then
      fail_count=$((fail_count + 1))
    fi
  done

  if (( done_count == GVA_MANAGER_NODE_COUNT )); then
    validate_manager_logs
    echo "$LOG_PREFIX PASS: all managers completed"
    for node_name in "${NODE_NAMES[@]}"; do
      print_node_summary "$node_name"
    done
    exit 0
  fi
  if (( fail_count > 0 )); then
    if [[ "$EXPECT_FAILURE" == "1" ]]; then
      validate_expected_failure
      echo "$LOG_PREFIX PASS: expected manager failure observed"
      exit 0
    fi
    echo "$LOG_PREFIX FAIL: manager reported failure" >&2
    for node_name in "${NODE_NAMES[@]}"; do
      print_node_summary "$node_name" >&2 || true
    done
    exit 1
  fi
  sleep 0.5
done

echo "$LOG_PREFIX FAIL: timeout waiting for completion" >&2
for node_name in "${NODE_NAMES[@]}"; do
  print_node_summary "$node_name" >&2 || true
done
exit 1
