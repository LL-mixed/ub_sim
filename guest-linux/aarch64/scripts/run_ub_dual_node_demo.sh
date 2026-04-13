#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_demo}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-/Volumes/repos/pypto_workspace/simulator/vendor/ub_topology_two_node_v0.ini}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}"
RUN_SECS="${RUN_SECS:-180}"
ITERATIONS="${ITERATIONS:-1}"
START_GAP_SECS="${START_GAP_SECS:-3}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_ub_chat=1 linqu_ub_rpc_demo=1}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-/Volumes/repos/pypto_workspace/simulator/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
QMP_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}/qmp"
MIN_PASS_RATE_PERCENT="${MIN_PASS_RATE_PERCENT:-100}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/demo_report.latest.txt}"
MAX_RUNTIME="${MAX_RUNTIME:-300}"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_${RANDOM}}"
MAIN_PID=$$

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

# Keep init alive after probes so the harness can terminate QEMU directly.
# This avoids guest shutdown/remove path stacktraces that are unrelated to
# chat/rpc/rdma dataplane validation.
if [[ "$APPEND_EXTRA" != *"linqu_probe_hold="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} linqu_probe_hold=1"
fi

timeout_watchdog() {
  local timeout_sec="$1"
  sleep "$timeout_sec"
  echo "global timeout ${timeout_sec}s reached, terminating test" >&2
  kill -TERM "$MAIN_PID" 2>/dev/null || true
}

timeout_watchdog "$MAX_RUNTIME" &
WATCHDOG_PID=$!
trap 'kill "$WATCHDOG_PID" 2>/dev/null || true; cleanup_all_demo_pid_files' EXIT INT TERM

cleanup_pid() {
  local pid_file="$1"
  local pid=""
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file" 2>/dev/null || true)"
  fi
  if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -f "$pid_file"
}

cleanup_all_demo_pid_files() {
  local pid_file=""
  for pid_file in "$OUT_DIR"/ub_nodeA.demo.*.pid "$OUT_DIR"/ub_nodeB.demo.*.pid; do
    cleanup_pid "$pid_file"
  done
}

wait_for_log_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]] && rg -q "$pattern" "$file"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

wait_for_log_pass_or_fail() {
  local file="$1"
  local pass_pattern="$2"
  local fail_pattern="$3"
  local timeout_s="$4"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      if rg -q "$pass_pattern" "$file"; then
        return 0
      fi
      if rg -q "$fail_pattern" "$file"; then
        return 1
      fi
    fi
    sleep 0.2
  done
  return 2
}

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! rg -q "$pattern" "$file"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_absent() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if rg -q "$pattern" "$file"; then
    echo "unexpected log marker: $label in $file" >&2
    return 1
  fi
}

validate_chat_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_chat\\] pass" "${node_name} chat pass" || return 1
  assert_log_absent "$log_file" "\\[ub_chat\\] fail" "${node_name} chat fail" || return 1
  assert_log_has "$log_file" "\\[ub_chat\\] summary tx=5 rx=5" "${node_name} chat tx/rx summary" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[CHAT\\] nodeA seq=[0-9]+ \"copy, greeting back from NodeB\"" \
      "${node_name} chat reply payload" || return 1
  else
    assert_log_has "$log_file" "\\[CHAT\\] nodeB seq=[0-9]+ \"greeting from NodeA\"" \
      "${node_name} chat request payload" || return 1
  fi
}

validate_rpc_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_rpc\\] pass" "${node_name} rpc pass" || return 1
  assert_log_absent "$log_file" "\\[ub_rpc\\] fail" "${node_name} rpc fail" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[RPC\\] client op=ECHO msg_id=1 status=OK result=\"greeting from NodeA\" expected=\"greeting from NodeA\" verified=1" \
      "${node_name} rpc echo semantic" || return 1
    assert_log_has "$log_file" "\\[RPC\\] client op=CRC32 msg_id=2 status=OK payload=\"buffer from NodeA for CRC verification over ub_link\" result=\"0x[0-9a-f]{8}\" expected=\"0x[0-9a-f]{8}\" verified=1" \
      "${node_name} rpc crc semantic" || return 1
  else
    assert_log_has "$log_file" "\\[RPC\\] server handled op=ECHO msg_id=1" \
      "${node_name} rpc server echo handled" || return 1
    assert_log_has "$log_file" "\\[RPC\\] server handled op=CRC32 msg_id=2" \
      "${node_name} rpc server crc handled" || return 1
  fi
}

validate_rdma_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_rdma\\] pass" "${node_name} rdma pass" || return 1
  assert_log_absent "$log_file" "\\[ub_rdma\\] fail" "${node_name} rdma fail" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 2: alloc_ummu_tid -> ok" \
    "${node_name} rdma alloc ummu tid" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 2: alloc_token_id -> ok" \
    "${node_name} rdma alloc token id" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 7: register_seg -> ok" \
    "${node_name} rdma register seg" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 8: import_jetty -> ok" \
    "${node_name} rdma import jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9: bind_jetty -> ok" \
    "${node_name} rdma bind jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: post_recv -> ok" \
    "${node_name} rdma post recv" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: ready_sync -> ok" \
    "${node_name} rdma ready sync" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: send_request -> ok len=[0-9]+" \
      "${node_name} rdma send request" || return 1
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: recv_reply -> ok payload=\"rdma reply payload from NodeB\"" \
      "${node_name} rdma reply payload" || return 1
  else
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: recv_request -> ok payload=\"rdma request payload from NodeA\"" \
      "${node_name} rdma request payload" || return 1
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: send_reply -> ok len=[0-9]+" \
      "${node_name} rdma send reply" || return 1
  fi
  assert_log_has "$log_file" "\\[ub_rdma\\] step 10: unbind_jetty -> ok" \
    "${node_name} rdma unbind jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 10: unimport_jetty -> ok" \
    "${node_name} rdma unimport jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: unregister_seg -> ok" \
    "${node_name} rdma unregister seg cleanup" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: free_token_id -> ok" \
    "${node_name} rdma free token id cleanup" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: free_ummu_tid -> ok" \
    "${node_name} rdma free ummu tid cleanup" || return 1
  assert_log_absent "$log_file" "UDMA: invalid port speed = 0" \
    "${node_name} rdma invalid port speed" || return 1
  assert_log_absent "$log_file" "failed to query device status" \
    "${node_name} rdma query device status failure" || return 1
  assert_log_absent "$log_file" "ubcore topo map doesn't exist" \
    "${node_name} rdma topo map missing" || return 1
  assert_log_absent "$log_file" "UDMA: wait resp timeout" \
    "${node_name} rdma wait response timeout" || return 1
  assert_log_absent "$log_file" "fail to notify mue save tp" \
    "${node_name} rdma save tp failure" || return 1
  assert_log_absent "$log_file" "ubcore_unimport_jetty_async failed" \
    "${node_name} rdma unimport jetty async failure" || return 1
  assert_log_absent "$log_file" "failed to remove uobject" \
    "${node_name} rdma uobject cleanup failure" || return 1
  assert_log_absent "$log_file" "invalidate cfg_table failed" \
    "${node_name} rdma cfg table cleanup failure" || return 1
}

validate_obmm_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_has "$log_file" "\\[ub_obmm\\] pass" "${node_name} obmm pass" || return 1
  assert_log_absent "$log_file" "\\[ub_obmm\\] fail" "${node_name} obmm fail" || return 1
  if [[ "$node_name" == "nodeA" ]]; then
    assert_log_has "$log_file" "\\[ub_obmm\\] export -> ok mem_id=[0-9]+ uba=0x[0-9a-f]+ token=[0-9]+" \
      "${node_name} obmm export" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] sync: nodeB import acknowledged" \
      "${node_name} obmm import ack" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] nodeA verify nodeB write -> ok payload=\"obmm-import-payload-from-nodeB\"" \
      "${node_name} obmm verify nodeB write" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] sync: nodeB unimport acknowledged" \
      "${node_name} obmm unimport ack" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] unexport -> ok mem_id=[0-9]+" \
      "${node_name} obmm unexport" || return 1
  else
    assert_log_has "$log_file" "\\[ub_obmm\\] mem_window mar=[0-9]+ decode=0x[0-9a-f]+ cc=\\[0x[0-9a-f]+,0x[0-9a-f]+\\]" \
      "${node_name} obmm mem window" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] import -> ok mem_id=[0-9]+ local_pa=0x[0-9a-f]+ local_cna=0x[0-9a-f]+ remote_cna=0x[0-9a-f]+" \
      "${node_name} obmm import" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] nodeB verify nodeA write -> ok payload=\"obmm-export-payload-from-nodeA\"" \
      "${node_name} obmm verify nodeA write" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] sync: nodeA writeback acknowledged" \
      "${node_name} obmm writeback ack" || return 1
    assert_log_has "$log_file" "\\[ub_obmm\\] unimport -> ok mem_id=[0-9]+" \
      "${node_name} obmm unimport" || return 1
  fi
}

validate_kernel_health_log() {
  local node_name="$1"
  local log_file="$2"
  assert_log_absent "$log_file" "WARNING: CPU:" "${node_name} kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "${node_name} stacktrace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "${node_name} panic" || return 1
}

ipourma_ipv4_args_for_role() {
  local role="$1"
  case "$role" in
    nodeA)
      echo "linqu_ipourma_ipv4=10.0.0.1 linqu_ipourma_peer_ipv4=10.0.0.2"
      ;;
    nodeB)
      echo "linqu_ipourma_ipv4=10.0.0.2 linqu_ipourma_peer_ipv4=10.0.0.1"
      ;;
    *)
      ;;
  esac
}

start_node() {
  local node_id="$1"
  local role="$2"
  local guest_log="$3"
  local qemu_log="$4"
  local pid_file="$5"
  local qmp_socket="$6"
  local qemu_extra=()
  local node_append_extra="$APPEND_EXTRA"
  local ipourma_args=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  ipourma_args="$(ipourma_ipv4_args_for_role "$role")"
  if [[ -n "$ipourma_args" ]]; then
    node_append_extra="${node_append_extra} ${ipourma_args}"
  fi

  mkdir -p "$(dirname "$qmp_socket")"
  mkdir -p "$(dirname "$guest_log")"
  mkdir -p "$(dirname "$qemu_log")"

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
      -append "console=ttyAMA0 rdinit=${RDINIT} linqu_urma_dp_role=${role} ${node_append_extra}" \
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

  echo "Waiting for FM links to be ready (timeout ${timeout_s}s)..."

  while (( SECONDS < deadline )); do
    local nodea_ready=false
    local nodeb_ready=false

    if [[ -f "$nodea_status" ]]; then
      local state=$(grep "^state=" "$nodea_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then
        nodea_ready=true
      fi
    fi
    if [[ "$nodea_ready" == "false" ]] && [[ -f "$nodea_log" ]] && \
       rg -q "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodea_log"; then
      nodea_ready=true
    fi

    if [[ -f "$nodeb_status" ]]; then
      local state=$(grep "^state=" "$nodeb_status" 2>/dev/null | cut -d'=' -f2)
      if [[ "$state" == "READY" ]]; then
        nodeb_ready=true
      fi
    fi
    if [[ "$nodeb_ready" == "false" ]] && [[ -f "$nodeb_log" ]] && \
       rg -q "marked connected for ubcdev0:1 state=1 socket=1 guid_valid=1 snapshot_reconciled=1" "$nodeb_log"; then
      nodeb_ready=true
    fi

    if [[ "$nodea_ready" == "true" && "$nodeb_ready" == "true" ]]; then
      echo "FM links ready!"
      return 0
    fi

    sleep 0.2
  done

  echo "FM links NOT ready within timeout!" >&2
  echo "=== nodeA link status ===" >&2
  if [[ -f "$nodea_status" ]]; then
    cat "$nodea_status" >&2
  else
    echo "(no status file: $nodea_status)" >&2
  fi
  echo "=== nodeB link status ===" >&2
  if [[ -f "$nodeb_status" ]]; then
    cat "$nodeb_status" >&2
  else
    echo "(no status file: $nodeb_status)" >&2
  fi
  return 1
}

check_entity_ready() {
  local node="$1"
  local log_file="$2"
  local timeout_sec="${3:-30}"
  local expected_count="${4:-2}"

  echo "Checking entity readiness on ${node} (timeout ${timeout_sec}s, expected ${expected_count} entities)..."

  local elapsed=0
  while [ $elapsed -lt $timeout_sec ]; do
    if [[ -f "$log_file" ]]; then
      local count=$(rg -c "entity_reg inject SUCCESS|entity_table_init:.*state=present|entity_plan: loaded entity .* state=present" "$log_file" 2>/dev/null || echo "0")
      if [ "$count" -ge "$expected_count" ]; then
        echo "PASS: Entities ready on ${node} (${count} entities)"
        return 0
      fi
    fi
    sleep 0.5
    elapsed=$((elapsed + 1))
  done

  echo "FAIL: Entities not ready on ${node} after ${timeout_sec}s"
  return 1
}

dump_link_diagnostics() {
  local nodea_log="$1"
  local nodeb_log="$2"
  local nodea_status="$SHARED_DIR/nodeA_ubcdev0__1.status"
  local nodeb_status="$SHARED_DIR/nodeB_ubcdev0__1.status"

  echo "=== Link Diagnostics ===" >&2
  printf "%-10s %-10s %-15s %-15s %-10s\n" "Node" "Socket" "RemoteGUID" "State" "Error" >&2
  printf "%-10s %-10s %-15s %-15s %-10s\n" "----" "------" "-----------" "-----" "-----" >&2

  for node in "nodeA" "nodeB"; do
    local status_file="${SHARED_DIR}/${node}_ubcdev0__1.status"
    if [[ -f "$status_file" ]]; then
      local socket=$(grep "^socket_connected=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local guid=$(grep "^remote_guid_valid=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local state=$(grep "^state=" "$status_file" 2>/dev/null | cut -d'=' -f2)
      local error=$(grep "^last_error=" "$status_file" 2>/dev/null | cut -d'=' -f2 | sed 's/"//g')

      socket=${socket:-false}
      guid=${guid:-false}
      state=${state:-UNKNOWN}
      error=${error:-none}

      printf "%-10s %-10s %-15s %-15s %-10s\n" \
        "$node" "$socket" "$guid" "$state" "$error" >&2
    else
      printf "%-10s %-10s %-15s %-15s %-10s\n" \
        "$node" "N/A" "N/A" "NO_STATUS" "missing_file" >&2
    fi
  done

  echo "=== Recent log markers ===" >&2
  echo "nodeA:" >&2
  rg -n "ub_link:|ub_fm:" "$nodea_log" 2>/dev/null | tail -10 >&2 || true
  echo "nodeB:" >&2
  rg -n "ub_link:|ub_fm:" "$nodeb_log" 2>/dev/null | tail -10 >&2 || true
}

cont_qemu() {
  local qmp_socket="$1"
  local node_name="$2"
  local timeout_s="${3:-10}"

  echo "Resuming $node_name via QMP ($qmp_socket)..."

  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -S "$qmp_socket" ]]; then
      break
    fi
    sleep 0.1
  done

  if [[ ! -S "$qmp_socket" ]]; then
    echo "QMP socket not ready: $qmp_socket" >&2
    return 0
  fi

  python3 -c "
import socket
import json
import sys
import traceback
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect('${qmp_socket}')
    s.recv(1024)
    s.sendall(b'{\\\"execute\\\": \\\"qmp_capabilities\\\"}\r\n')
    s.recv(1024)
    s.sendall(b'{\\\"execute\\\": \\\"cont\\\"}\r\n')
    resp = s.recv(1024)
    s.close()
    if b'return' in resp or b'error' in resp or b'CommandNotFound' in resp:
        sys.exit(0)
    else:
        sys.exit(0)
except Exception as e:
    sys.exit(0)
" 2>/dev/null || true

  if [[ $? -eq 0 ]]; then
    echo "$node_name resumed"
  else
    echo "QMP resume had issues, but continuing anyway" >&2
  fi
  return 0
}

check_link_early_or_fail() {
  local nodea_log="$1"
  local nodeb_log="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  local fail_pat="ub_link: server listen failed|ub_link: failed to connect remote server|bizmsg roundtrip fail: remote linkup not ready|\\[init\\] ub sysfs wait timed out|Failed to bind socket|Failed to bind|failed to create listener|Address already in use|Operation not permitted"
  local ok_pat="ub_link: connected to remote server|ub_link: accepted connection|remote snapshot load done|remote cfg notify done"

  while (( SECONDS < deadline )); do
    if [[ -f "$nodea_log" ]] && rg -q "$fail_pat" "$nodea_log"; then
      echo "early qemu/link failure detected on nodeA" >&2
      dump_link_diagnostics "$nodea_log" "$nodeb_log"
      return 1
    fi
    if [[ -f "$nodeb_log" ]] && rg -q "$fail_pat" "$nodeb_log"; then
      echo "early qemu/link failure detected on nodeB" >&2
      dump_link_diagnostics "$nodea_log" "$nodeb_log"
      return 1
    fi

    if [[ -f "$nodea_log" && -f "$nodeb_log" ]] &&
       rg -q "$ok_pat" "$nodea_log" &&
       rg -q "$ok_pat" "$nodeb_log"; then
      return 0
    fi

    sleep 0.2
  done

  return 0
}

run_iteration() {
  local iter="$1"
  local iter_log_dir="$LOG_DIR/${RUN_ID}_demo_iter${iter}"
  local nodea_guest_log="$iter_log_dir/nodeA_guest.log"
  local nodeb_guest_log="$iter_log_dir/nodeB_guest.log"
  local nodea_qemu_log="$iter_log_dir/nodeA_qemu.log"
  local nodeb_qemu_log="$iter_log_dir/nodeB_qemu.log"
  local nodea_log_link="$OUT_DIR/ub_nodeA.demo.${iter}.log"
  local nodeb_log_link="$OUT_DIR/ub_nodeB.demo.${iter}.log"
  local nodea_qemu_log_link="$OUT_DIR/ub_nodeA.demo.${iter}.qemu.log"
  local nodeb_qemu_log_link="$OUT_DIR/ub_nodeB.demo.${iter}.qemu.log"
  local nodea_pid_file="$OUT_DIR/ub_nodeA.demo.${iter}.pid"
  local nodeb_pid_file="$OUT_DIR/ub_nodeB.demo.${iter}.pid"
  local nodea_qmp="$QMP_DIR/nodeA.${iter}.sock"
  local nodeb_qmp="$QMP_DIR/nodeB.${iter}.sock"
  local chat_enabled=0
  local rpc_enabled=0
  local rdma_enabled=0
  local obmm_enabled=0
  local stale_files=()

  if [[ "$APPEND_EXTRA" == *"linqu_ub_chat=1"* ]]; then
    chat_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_rpc_demo=1"* ]]; then
    rpc_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_rdma_demo=1"* ]]; then
    rdma_enabled=1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_demo=1"* ]]; then
    obmm_enabled=1
  fi

  rm -f /tmp/ub-qemu/ub-bus-instance-*.lock
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

  mkdir -p "$OUT_DIR"
  mkdir -p "$LOG_DIR"
  mkdir -p "$iter_log_dir"
  mkdir -p "$SHARED_DIR"
  mkdir -p "$QMP_DIR"
  stale_files=("$SHARED_DIR"/*.ini "$SHARED_DIR"/*.kick "$SHARED_DIR"/*.lock)
  if (( ${#stale_files[@]} )); then
    rm -f "${stale_files[@]}"
  fi
  rm -f "$nodea_guest_log" "$nodeb_guest_log" "$nodea_qemu_log" "$nodeb_qemu_log"
  ln -sfn "$nodea_guest_log" "$nodea_log_link"
  ln -sfn "$nodeb_guest_log" "$nodeb_log_link"
  ln -sfn "$nodea_qemu_log" "$nodea_qemu_log_link"
  ln -sfn "$nodeb_qemu_log" "$nodeb_qemu_log_link"
  echo "iteration ${iter} logs: $iter_log_dir"

  echo "Starting nodeA (paused)..."
  start_node "nodeA" "nodeA" "$nodea_guest_log" "$nodea_qemu_log" "$nodea_pid_file" "$nodea_qmp"
  sleep 0.5
  echo "Starting nodeB (paused)..."
  start_node "nodeB" "nodeB" "$nodeb_guest_log" "$nodeb_qemu_log" "$nodeb_pid_file" "$nodeb_qmp"

  if ! check_link_early_or_fail "$nodea_qemu_log" "$nodeb_qemu_log" 10; then
    echo "iteration ${iter}: early link failure detected" >&2
    return 11
  fi

  cont_qemu "$nodea_qmp" "nodeA"
  cont_qemu "$nodeb_qmp" "nodeB"

  if ! wait_for_fm_links_ready "$nodea_qemu_log" "$nodeb_qemu_log" 30; then
    echo "iteration ${iter}: FM links failed to reach READY state within timeout" >&2
    return 11
  fi

  if [ "$ENTITY_COUNT" -gt "1" ]; then
    if ! check_entity_ready "nodeA" "$nodea_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeA entities not ready within timeout" >&2
      return 12
    fi
    if ! check_entity_ready "nodeB" "$nodeb_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeB entities not ready within timeout" >&2
      return 12
    fi
  fi

  sleep 1
  if ! kill -0 "$(cat "$nodea_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeA died after resume" >&2
    return 1
  fi
  if ! kill -0 "$(cat "$nodeb_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeB died after resume" >&2
    return 1
  fi

  if [[ "$chat_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[init\\] ub chat pass" "\\[init\\] ub chat fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA chat demo reported failure" >&2
        return 15
        ;;
      *)
        echo "iteration ${iter}: nodeA chat demo did not pass within ${RUN_SECS}s" >&2
        return 15
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[init\\] ub chat pass" "\\[init\\] ub chat fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB chat demo reported failure" >&2
        return 15
        ;;
      *)
        echo "iteration ${iter}: nodeB chat demo did not pass within ${RUN_SECS}s" >&2
        return 15
        ;;
    esac
  fi

  if [[ "$rpc_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[init\\] ub rpc demo pass" "\\[init\\] ub rpc demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA rpc demo reported failure" >&2
        return 13
        ;;
      *)
        echo "iteration ${iter}: nodeA rpc demo did not pass within ${RUN_SECS}s" >&2
        return 13
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[init\\] ub rpc demo pass" "\\[init\\] ub rpc demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB rpc demo reported failure" >&2
        return 13
        ;;
      *)
        echo "iteration ${iter}: nodeB rpc demo did not pass within ${RUN_SECS}s" >&2
        return 13
        ;;
    esac
  fi

  if [[ "$rdma_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[init\\] ub rdma demo pass" "\\[init\\] ub rdma demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA rdma demo reported failure" >&2
        return 14
        ;;
      *)
        echo "iteration ${iter}: nodeA rdma demo did not finish within ${RUN_SECS}s" >&2
        return 14
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[init\\] ub rdma demo pass" "\\[init\\] ub rdma demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB rdma demo reported failure" >&2
        return 14
        ;;
      *)
        echo "iteration ${iter}: nodeB rdma demo did not finish within ${RUN_SECS}s" >&2
        return 14
        ;;
    esac
  fi

  if [[ "$obmm_enabled" -eq 1 ]]; then
    wait_for_log_pass_or_fail "$nodea_guest_log" "\\[init\\] ub obmm demo pass" "\\[init\\] ub obmm demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeA obmm demo reported failure" >&2
        return 16
        ;;
      *)
        echo "iteration ${iter}: nodeA obmm demo did not finish within ${RUN_SECS}s" >&2
        return 16
        ;;
    esac

    wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[init\\] ub obmm demo pass" "\\[init\\] ub obmm demo fail" "$RUN_SECS"
    case "$?" in
      0) ;;
      1)
        echo "iteration ${iter}: nodeB obmm demo reported failure" >&2
        return 16
        ;;
      *)
        echo "iteration ${iter}: nodeB obmm demo did not finish within ${RUN_SECS}s" >&2
        return 16
        ;;
    esac
  fi

  sleep 1
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

  echo "=== nodeA guest(demo:${iter}) ==="
  tail -n 120 "$nodea_guest_log"
  echo "=== nodeB guest(demo:${iter}) ==="
  tail -n 120 "$nodeb_guest_log"
  echo "=== nodeA qemu(demo:${iter}) ==="
  tail -n 80 "$nodea_qemu_log"
  echo "=== nodeB qemu(demo:${iter}) ==="
  tail -n 80 "$nodeb_qemu_log"

  if [[ "$chat_enabled" -eq 1 ]]; then
    validate_chat_log "nodeA" "$nodea_guest_log" || return 1
    validate_chat_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$rpc_enabled" -eq 1 ]]; then
    validate_rpc_log "nodeA" "$nodea_guest_log" || return 1
    validate_rpc_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_ub_rdma_demo=1"* ]]; then
    validate_rdma_log "nodeA" "$nodea_guest_log" || return 1
    validate_rdma_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  if [[ "$APPEND_EXTRA" == *"linqu_obmm_demo=1"* ]]; then
    validate_obmm_log "nodeA" "$nodea_guest_log" || return 1
    validate_obmm_log "nodeB" "$nodeb_guest_log" || return 1
  fi
  validate_kernel_health_log "nodeA" "$nodea_guest_log" || return 1
  validate_kernel_health_log "nodeB" "$nodeb_guest_log" || return 1

  echo "iteration ${iter}: dual-node demo pass"
}

declare -a ITERATION_RESULTS
declare -a ITERATION_ERRORS

for ((i = 1; i <= ITERATIONS; i++)); do
  if run_iteration "$i"; then
    ITERATION_RESULTS[$i]=0
  else
    ret=$?
    ITERATION_RESULTS[$i]=$ret
    ITERATION_ERRORS[$i]="iteration $i failed with exit code $ret"
  fi
done

echo "=== Test Results ===" >&2
passed=0
failed=0
for ((i = 1; i <= ITERATIONS; i++)); do
  if [[ ${ITERATION_RESULTS[$i]:-255} -eq 0 ]]; then
    passed=$((passed + 1))
    echo "iteration $i: PASS"
  else
    failed=$((failed + 1))
    echo "iteration $i: FAIL (exit code ${ITERATION_RESULTS[$i]})" >&2
    if [[ -n "${ITERATION_ERRORS[$i]}" ]]; then
      echo "  ${ITERATION_ERRORS[$i]}" >&2
    fi
  fi
done

echo "=== Summary ===" >&2
echo "Passed: $passed / $ITERATIONS" >&2
echo "Failed: $failed / $ITERATIONS" >&2

pass_rate=$((passed * 100 / ITERATIONS))
echo "Pass rate: ${pass_rate}% (required >= ${MIN_PASS_RATE_PERCENT}%)" >&2

{
  echo "scenario=dual-node-demo"
  echo "iterations=${ITERATIONS}"
  echo "run_secs=${RUN_SECS}"
  echo "start_gap_secs=${START_GAP_SECS}"
  echo "run_id=${RUN_ID}"
  echo "logs_dir=${LOG_DIR}"
  echo "min_pass_rate_percent=${MIN_PASS_RATE_PERCENT}"
  echo "max_runtime=${MAX_RUNTIME}"
  echo "passed=${passed}"
  echo "failed=${failed}"
  echo "pass_rate_percent=${pass_rate}"
  for ((i = 1; i <= ITERATIONS; i++)); do
    echo "iteration_${i}_result=${ITERATION_RESULTS[$i]:-255}"
  done
} > "$REPORT_FILE"
echo "Report: $REPORT_FILE" >&2

if (( pass_rate < MIN_PASS_RATE_PERCENT )); then
  echo "dual-node demo validation FAILED" >&2
  exit 1
fi

echo "dual-node demo validation passed (${ITERATIONS} iterations)"
exit 0
