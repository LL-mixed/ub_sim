#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
GSVA_MATRIX_NODE_COUNT="${GSVA_MATRIX_NODE_COUNT:-4}"
case "$GSVA_MATRIX_NODE_COUNT" in
  4)
    DEFAULT_TOPOLOGY_FILE="$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh.ini"
    NODE_NAMES=(nodeA nodeB nodeC nodeD)
    ;;
  8)
    DEFAULT_TOPOLOGY_FILE="$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini"
    NODE_NAMES=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
    ;;
  *)
    echo "[gsva-matrix] FAIL: GSVA_MATRIX_NODE_COUNT must be 4 or 8" >&2
    exit 1
    ;;
esac
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$DEFAULT_TOPOLOGY_FILE}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
PORT_NUM="${UB_SIM_PORT_NUM:-$((GSVA_MATRIX_NODE_COUNT - 1))}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-gsva-matrix${GSVA_MATRIX_NODE_COUNT}}"
QMP_DIR="$SHARED_DIR/qmp"
RUN_SECS="${RUN_SECS:-240}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_matrix${GSVA_MATRIX_NODE_COUNT}_${RANDOM}}"
GSVA_MATRIX_BASE="${GSVA_MATRIX_BASE:-0x700000000000}"
GSVA_MATRIX_SLICE_SIZE="${GSVA_MATRIX_SLICE_SIZE:-0x400000}"
GSVA_MODE="${GSVA_MODE:-arm_mmu}"
GSVA_STRICT="${GSVA_STRICT:-1}"
LOG_PREFIX="[gsva-matrix${GSVA_MATRIX_NODE_COUNT}]"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

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
  echo "$OUT_DIR/ub_$1.gsva_matrix${GSVA_MATRIX_NODE_COUNT}.${RUN_ID}.pid"
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
  local node_cna
  local qmp_socket
  local qemu_extra=()

  node_cna="$(node_cna_for "$node_idx")"
  qmp_socket="$(qmp_socket_for "$node_name")"
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
      -serial file:"$(guest_log_for "$node_name")" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo obmm_gsva_demo linqu_urma_dp_role=${node_name} linqu_node_idx=${node_idx} linqu_cna=${node_cna} obmm_gsva_mode=matrix obmm_gsva_base=${GSVA_MATRIX_BASE} obmm_gsva_size=${GSVA_MATRIX_SLICE_SIZE} obmm_gsva_node_count=${GSVA_MATRIX_NODE_COUNT} ${APPEND_EXTRA}" \
      >"$(qemu_log_for "$node_name")" 2>&1 &
  echo $! > "$(pid_file_for "$node_name")"
}

hex_lower() {
  printf '%x' "$(( $1 ))"
}

hex_prefixed() {
  printf '%#x' "$(( $1 ))"
}

matrix_value() {
  local writer="$1"
  local owner="$2"
  printf '0x%x' "$((0x4753564d00000000 | (writer << 8) | owner))"
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
  local expected=$((GSVA_MATRIX_NODE_COUNT * (GSVA_MATRIX_NODE_COUNT - 1)))
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

validate_mp_table_log() {
  local node_name="$1"
  local qemu_log="$2"
  local expected_routes="$3"

  awk -v expected="$expected_routes" -v node="$node_name" -v prefix="$LOG_PREFIX" '
    /^GSVA_MAP: map_id=/ && /source=2 profile=1/ {
      route_count++
      seg = ""
      for (i = 1; i <= NF; i++) {
        split($i, kv, "=")
        if (kv[1] == "segment_id") {
          seg = kv[2]
        }
      }
      if (seg != "") {
        seg_seen[seg] = 1
      }
    }
    END {
      for (seg in seg_seen) {
        seg_count++
      }
      if (route_count < expected) {
        printf "%s FAIL: %s QEMU log has too few GSVA routes: %d\n",
               prefix, node, route_count > "/dev/stderr"
        exit 2
      }
      if (seg_count < expected) {
        printf "%s FAIL: %s QEMU log does not cover unique peer GSVA segments: %d\n",
               prefix, node, seg_count > "/dev/stderr"
        exit 5
      }
    }
  ' "$qemu_log"
}

validate_node_logs() {
  local node_name
  local node_idx=0
  local guest_log
  local qemu_log
  local slice_base
  local expected_node0
  local expected_last

  for node_name in "${NODE_NAMES[@]}"; do
    guest_log="$(guest_log_for "$node_name")"
    qemu_log="$(qemu_log_for "$node_name")"
    slice_base="$((GSVA_MATRIX_BASE + node_idx * GSVA_MATRIX_SLICE_SIZE))"
    expected_node0="$(matrix_value 0 "$node_idx")"
    expected_last="$(matrix_value "$((GSVA_MATRIX_NODE_COUNT - 1))" "$node_idx")"

    if ! grep -q '\[obmm_gsva_demo\] kernel aperture registry -> ok' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks kernel aperture evidence" >&2
      return 1
    fi
    if ! grep -q "\[obmm_gsva_demo\] result=done mode=matrix node=${node_idx} node_count=${GSVA_MATRIX_NODE_COUNT}" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name did not complete matrix mode" >&2
      return 1
    fi
    if ! grep -q "slice_base=$(hex_prefixed "$slice_base") ptr=$(hex_prefixed "$slice_base")" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name did not preserve expected GSVA pointer" >&2
      return 1
    fi
    if ! grep -q "value_from_node0=${expected_node0}" "$guest_log" ||
       ! grep -q "value_from_last=${expected_last}" "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name lacks full-mesh write evidence" >&2
      return 1
    fi

    if ! validate_mp_table_log "$node_name" "$qemu_log" "$((GSVA_MATRIX_NODE_COUNT - 1))"; then
      return 1
    fi
    if [[ "$GSVA_MODE" == "arm_mmu" ]]; then
      if ! grep -q 'GSVA_TLB: lookup' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name ARM MMU mode lacks GSVA_TLB lookup evidence" >&2
        return 1
      fi
      if ! grep -q 'GSVA_COH:' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name ARM MMU mode lacks GSVA_COH evidence" >&2
        return 1
      fi
      if grep -q 'GVA_TCG_TRANSLATE' "$qemu_log"; then
        echo "$LOG_PREFIX FAIL: $node_name ARM MMU mode unexpectedly used SIM_GVA_TCG data path" >&2
        return 1
      fi
    fi
    if grep -qE '\[obmm_gsva_demo\] result=fail|Kernel panic - not syncing|Call trace:' "$guest_log"; then
      echo "$LOG_PREFIX FAIL: $node_name has failure markers" >&2
      return 1
    fi

    node_idx=$((node_idx + 1))
  done
}

print_node_summary() {
  local node_name="$1"
  echo "$LOG_PREFIX ${node_name}:"
  grep '\[obmm_gsva_demo\]' "$(guest_log_for "$node_name")" | tail -6
}

echo "$LOG_PREFIX run_id=$RUN_ID base=$GSVA_MATRIX_BASE slice_size=$GSVA_MATRIX_SLICE_SIZE"
echo "$LOG_PREFIX topology=$TOPOLOGY_FILE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP port_num=$PORT_NUM"
echo "$LOG_PREFIX starting ${GSVA_MATRIX_NODE_COUNT} nodes..."

integer idx=0
for node_name in "${NODE_NAMES[@]}"; do
  start_node "$node_name" "$idx"
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

echo "$LOG_PREFIX waiting for matrix completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  done_count=0
  fail_count=0
  for node_name in "${NODE_NAMES[@]}"; do
    if [[ -f "$(guest_log_for "$node_name")" ]] &&
       grep -qE '\[obmm_gsva_demo\] result=done mode=matrix' "$(guest_log_for "$node_name")"; then
      done_count=$((done_count + 1))
    fi
    if [[ -f "$(guest_log_for "$node_name")" ]] &&
       grep -qE '\[obmm_gsva_demo\] result=fail|\[run_demo\] linqu_ub_obmm_gsva_demo failed|Kernel panic - not syncing' "$(guest_log_for "$node_name")"; then
      fail_count=$((fail_count + 1))
    fi
  done

  if (( done_count == GSVA_MATRIX_NODE_COUNT )); then
    validate_node_logs
    echo "$LOG_PREFIX PASS: all nodes completed"
    for node_name in "${NODE_NAMES[@]}"; do
      print_node_summary "$node_name"
    done
    exit 0
  fi
  if (( fail_count > 0 )); then
    echo "$LOG_PREFIX FAIL: matrix demo reported failure" >&2
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
