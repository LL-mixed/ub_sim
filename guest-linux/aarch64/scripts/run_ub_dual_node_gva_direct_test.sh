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
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-gva-direct}"
RUN_SECS="${RUN_SECS:-120}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gva_direct_${RANDOM}}"
GVA_DIRECT_MODE="${GVA_DIRECT_MODE:-write-read}"
GVA_DIRECT_LOCAL_VA="${GVA_DIRECT_LOCAL_VA:-0x710000000000}"
GVA_DIRECT_HOME_VA="${GVA_DIRECT_HOME_VA:-0x720000000000}"
GVA_DIRECT_SIZE="${GVA_DIRECT_SIZE:-0x400000}"

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
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.gva_direct.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.gva_direct.${RUN_ID}.pid"

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
      -append "console=ttyAMA0 rdinit=/bin/run_demo gva_direct_demo linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} gva_direct_mode=${GVA_DIRECT_MODE} gva_direct_local_va=${GVA_DIRECT_LOCAL_VA} gva_direct_home_va=${GVA_DIRECT_HOME_VA} gva_direct_size=${GVA_DIRECT_SIZE} ${APPEND_EXTRA}" \
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

validate_gva_direct_logs() {
  local local_hex
  local home_hex

  local_hex="$(printf '%x' "$((GVA_DIRECT_LOCAL_VA))")"
  home_hex="$(printf '%x' "$((GVA_DIRECT_HOME_VA))")"

  if ! grep -q "\[gva_direct_demo\] result=done mode=${GVA_DIRECT_MODE} role=home" "$NODEA_GUEST_LOG" ||
     ! grep -q "\[gva_direct_demo\] result=done mode=${GVA_DIRECT_MODE} role=peer" "$NODEB_GUEST_LOG"; then
    echo "[gva-direct] FAIL: demo did not complete on both roles" >&2
    return 1
  fi
  if [[ "$GVA_DIRECT_MODE" == "invalid-cache" ]]; then
    if ! grep -q '\[gva_direct_demo\] result=done mode=invalid-cache role=peer bad_cache_policy=0xffffffff' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: invalid-cache mode did not report rejected cache policy" >&2
      return 1
    fi
    if ! grep -q 'SIM_DEC: GVA unsupported cache_policy=4294967295' "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-cache mode lacks QEMU cache_policy rejection evidence" >&2
      return 1
    fi
    if grep -q 'GVA_S3_MAP' "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-cache mode unexpectedly programmed a GVA route" >&2
      return 1
    fi
    return 0
  fi
  if [[ "$GVA_DIRECT_MODE" == "overlap" ]]; then
    if ! grep -q '\[gva_direct_demo\] result=done mode=overlap role=peer' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: overlap mode did not report rejected second import" >&2
      return 1
    fi
    if ! grep -q 'OBMM: addr_check:failed to occupy PA range: ret=-EEXIST' "$NODEB_GUEST_LOG" ||
       ! grep -q 'OBMM: Failed to prepare import memory: ret=-EEXIST' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: overlap mode lacks OBMM PA overlap evidence" >&2
      return 1
    fi
    if ! grep -Eq "GVA_S3_MAP .*local_va=${local_hex} .*home_va=${home_hex} .*address_profile=1" "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: overlap mode did not create the first GVA route" >&2
      return 1
    fi
    return 0
  fi
  if [[ "$GVA_DIRECT_MODE" == "invalid-ptag" ]]; then
    if ! grep -q '\[gva_direct_demo\] result=done mode=invalid-ptag role=peer fault_injected=1' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: invalid-ptag mode did not report access fault injection" >&2
      return 1
    fi
    if ! grep -Eq "GVA_S3_MAP .*local_va=${local_hex} .*home_va=${home_hex} .*p_tag=4294967295 .*address_profile=1" "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-ptag mode did not create the fault-injected GVA route" >&2
      return 1
    fi
    if ! grep -q 'GVA_ROUTE_MISS reason=p_tag' "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-ptag mode lacks QEMU route miss evidence" >&2
      return 1
    fi
    return 0
  fi
  if [[ "$GVA_DIRECT_MODE" == "token-mismatch" ]]; then
    if ! grep -q '\[gva_direct_demo\] result=done mode=token-mismatch role=peer fault_injected=1' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: token-mismatch mode did not report access fault injection" >&2
      return 1
    fi
    if ! grep -Eq "GVA_S3_MAP .*local_va=${local_hex} .*home_va=${home_hex} .*address_profile=1" "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: token-mismatch mode did not create the GVA route" >&2
      return 1
    fi
    if ! grep -q 'GVA_FAULT reason=token_mismatch' "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: token-mismatch mode lacks QEMU token mismatch evidence" >&2
      return 1
    fi
    return 0
  fi
  if [[ "$GVA_DIRECT_MODE" == "invalid-upi" ]]; then
    if ! grep -q '\[gva_direct_demo\] result=done mode=invalid-upi role=peer fault_injected=1' "$NODEB_GUEST_LOG"; then
      echo "[gva-direct] FAIL: invalid-upi mode did not report access fault injection" >&2
      return 1
    fi
    if ! grep -Eq "GVA_S3_MAP .*local_va=${local_hex} .*home_va=${home_hex} .*address_profile=1" "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-upi mode did not create the GVA route" >&2
      return 1
    fi
    if ! grep -q 'GVA_FAULT reason=upi_mismatch' "$NODEB_QEMU_LOG"; then
      echo "[gva-direct] FAIL: invalid-upi mode lacks QEMU UPI mismatch evidence" >&2
      return 1
    fi
    return 0
  fi
  if ! grep -Eq "GVA_S3_MAP .*local_va=${local_hex} .*home_va=${home_hex} .*pte_offset=[1-9a-f][0-9a-f]* .*address_profile=1" "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing generic GVA_S3_MAP with nonzero pte_offset" >&2
    return 1
  fi
  if ! grep -Eq "GVA_ROUTE_DUMP state=active .*local_va=${local_hex} .*home_va=${home_hex} .*pte_offset=[1-9a-f][0-9a-f]* .*ma_table.dcna=.*mp_table.p_tag=.*address_profile=1" "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing active GVA route dump" >&2
    return 1
  fi
  if ! grep -Eq "GVA_ROUTE_DUMP state=active .*mp_table.ubc_port=1 .*mp_table.lane=1 .*mp_table.link_id=[1-9][0-9]*" "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing GVA mp_table port/lane/link evidence" >&2
    return 1
  fi
  if ! grep -Eq "UB SIM Decoder: gva map created .*local_va=${local_hex} home_va=${home_hex} pte_offset=[1-9a-f][0-9a-f]* address_profile=1" "$NODEB_GUEST_LOG"; then
    echo "[gva-direct] FAIL: missing guest GVA map metadata log" >&2
    return 1
  fi
  if ! grep -Eq 'GVA_PATH gva_path=cpu_window op=read ' "$NODEB_QEMU_LOG" ||
     ! grep -Eq 'GVA_PATH gva_path=cpu_window op=write ' "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing GVA cpu-window path read/write evidence" >&2
    return 1
  fi
  if ! grep -Eq 'GVA_STATS .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing nonzero GVA_STATS read/write evidence" >&2
    return 1
  fi
  if ! grep -Eq 'SIM_DEC_STATS .*gva_cpu_reads=[1-9][0-9]* .*gva_cpu_writes=[1-9][0-9]* .*cpu_reads=[1-9][0-9]* .*cpu_writes=[1-9][0-9]*' "$NODEB_QEMU_LOG"; then
    echo "[gva-direct] FAIL: missing nonzero SIM_DEC_STATS read/write evidence" >&2
    return 1
  fi
  case "$GVA_DIRECT_MODE" in
    sync)
      if ! grep -q '\[gva_direct_demo\] sync -> ok' "$NODEB_GUEST_LOG"; then
        echo "[gva-direct] FAIL: sync mode did not report sync success" >&2
        return 1
      fi
      ;;
    unmap-fault)
      if ! grep -q '\[gva_direct_demo\] unmap fault -> ok' "$NODEB_GUEST_LOG"; then
        echo "[gva-direct] FAIL: unmap-fault mode did not observe expected fault" >&2
        return 1
      fi
      ;;
    dump)
      if ! grep -Eq "\\[gva_direct_demo\\] guest_route_dump .*map_source=2 .*address_profile=1 .*local_va=${GVA_DIRECT_LOCAL_VA} .*home_va=${GVA_DIRECT_HOME_VA} .*pte_offset=0x[1-9a-f][0-9a-f]*" "$NODEB_GUEST_LOG"; then
        echo "[gva-direct] FAIL: dump mode did not emit guest route metadata" >&2
        return 1
      fi
      if ! grep -Eq "\\[gva_direct_demo\\] guest_proc_route_dump [0-9a-f]+ 1 2 1 0 0 [0-9a-f]+ [0-9a-f]+ ${local_hex} ${home_hex} [0-9a-f]+ [1-9a-f][0-9a-f]*" "$NODEB_GUEST_LOG"; then
        echo "[gva-direct] FAIL: dump mode did not emit proc GVA route metadata" >&2
        return 1
      fi
      ;;
  esac
}

echo "[gva-direct] run_id=$RUN_ID mode=$GVA_DIRECT_MODE local_va=$GVA_DIRECT_LOCAL_VA home_va=$GVA_DIRECT_HOME_VA size=$GVA_DIRECT_SIZE"
echo "[gva-direct] starting nodeA and nodeB..."

start_node nodeA nodeA 0 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB nodeB 1 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "[gva-direct] waiting for FM links..."
if ! wait_for_fm_links_ready "$NODEA_QEMU_LOG" "$NODEB_QEMU_LOG" "$LINK_WAIT_SECS"; then
  echo "[gva-direct] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[gva-direct] FM links ready"

echo "[gva-direct] waiting for demo completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$NODEA_GUEST_LOG" ]] && grep -qE '\[gva_direct_demo\] result=done' "$NODEA_GUEST_LOG" && \
     [[ -f "$NODEB_GUEST_LOG" ]] && grep -qE '\[gva_direct_demo\] result=done' "$NODEB_GUEST_LOG"; then
    validate_gva_direct_logs
    echo "[gva-direct] PASS: both nodes completed"
    echo "[gva-direct] nodeA:"
    grep '\[gva_direct_demo\]' "$NODEA_GUEST_LOG" | tail -8
    echo "[gva-direct] nodeB:"
    grep '\[gva_direct_demo\]' "$NODEB_GUEST_LOG" | tail -8
    exit 0
  fi
  if grep -qE '\[gva_direct_demo\] result=fail|\[run_demo\] linqu_gva_direct_demo failed' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
    echo "[gva-direct] FAIL: demo reported failure" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[gva-direct] FAIL: timeout waiting for completion" >&2
exit 1
