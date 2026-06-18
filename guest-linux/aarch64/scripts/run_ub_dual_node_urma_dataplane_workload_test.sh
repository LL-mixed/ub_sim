#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$REPO_ROOT/vendor/ub_topology_two_node_v0.ini}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}"
RUN_SECS="${RUN_SECS:-180}"
ITERATIONS="${ITERATIONS:-1}"
START_GAP_SECS="${START_GAP_SECS:-3}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-45}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
BASE_APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_bizmsg_verify=1 linqu_urma_dp_verify=1}"
RDINIT="${RDINIT:-/bin/run_app}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$REPO_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
QMP_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}/qmp"
BENCH_PKTS="${BENCH_PKTS:-0}"
BENCH_INTERVAL_US="${BENCH_INTERVAL_US:-1000}"
BENCH_WAIT_MS="${BENCH_WAIT_MS:-5000}"
BENCH_MIN_RX_PPS="${BENCH_MIN_RX_PPS:-0}"
BENCH_MAX_LOSS_PPM="${BENCH_MAX_LOSS_PPM:-1000000}"
BENCH_MIN_RX_PPS_GATE="${BENCH_MIN_RX_PPS_GATE:-$BENCH_MIN_RX_PPS}"
BENCH_MAX_LOSS_PPM_GATE="${BENCH_MAX_LOSS_PPM_GATE:-$BENCH_MAX_LOSS_PPM}"
MIN_PASS_RATE_PERCENT="${MIN_PASS_RATE_PERCENT:-100}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/urma_dp_workload_report.latest.txt}"
APPEND_EXTRA="$BASE_APPEND_EXTRA"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_${RANDOM}}"

source "$SCRIPT_DIR/qemu_ub_common.sh"
BASE_APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$BASE_APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$REPO_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if (( BENCH_PKTS > 0 )); then
  APPEND_EXTRA="$APPEND_EXTRA linqu_urma_dp_bench_pkts=${BENCH_PKTS} linqu_urma_dp_bench_interval_us=${BENCH_INTERVAL_US} linqu_urma_dp_bench_wait_ms=${BENCH_WAIT_MS} linqu_urma_dp_bench_min_rx_pps=${BENCH_MIN_RX_PPS} linqu_urma_dp_bench_max_loss_ppm=${BENCH_MAX_LOSS_PPM}"
fi

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

cleanup_all_urma_dp_pid_files() {
  local pid_file=""
  for pid_file in "$OUT_DIR"/ub_nodeA.urma_dp.*.pid "$OUT_DIR"/ub_nodeB.urma_dp.*.pid; do
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

extract_metric_from_summary() {
  local summary="$1"
  local key="$2"
  printf '%s\n' "$summary" | sed -nE "s/.*${key}=([0-9]+).*/\\1/p"
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

# Wait for FM links to be ready - check both status file AND log markers
# Workaround: ub_link_apply may overwrite mark_connected's READY state to PENDING
# due to state_age_ok using QEMU_CLOCK_VIRTUAL, so also accept log-based detection
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

    # Check nodeA: status file READY OR log shows connected+reconciled
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

    # Check nodeB: status file READY OR log shows connected+reconciled
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

# 快速检查实体是否就绪
check_entity_ready() {
  local node="$1"
  local log_file="$2"
  local timeout_sec="${3:-30}"
  local expected_count="${4:-2}"

  echo "Checking entity readiness on ${node} (timeout ${timeout_sec}s, expected ${expected_count} entities)..."

  local elapsed=0
  while [ $elapsed -lt $timeout_sec ]; do
    if [[ -f "$log_file" ]]; then
      # Accept either entity_reg injection, entity_table_init present state,
      # or entity_plan loaded present entries (for simulated entity plans).
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

# Resume paused QEMU instance via QMP
cont_qemu() {
  local qmp_socket="$1"
  local node_name="$2"
  local timeout_s="${3:-10}"

  echo "Resuming $node_name via QMP ($qmp_socket)..."

  # Wait for QMP socket to be ready
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -S "$qmp_socket" ]]; then
      break
    fi
    sleep 0.1
  done

  if [[ ! -S "$qmp_socket" ]]; then
    echo "QMP socket not ready: $qmp_socket" >&2
    # Don't fail - QEMU may already be running
    return 0
  fi

  # Send cont command via QMP using Python
  python3 -c "
import socket
import json
import sys
import traceback
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
try:
    s.connect('${qmp_socket}')
    # Read greeting
    s.recv(1024)
    # Send capabilities
    s.sendall(b'{\\\"execute\\\": \\\"qmp_capabilities\\\"}\r\n')
    s.recv(1024)
    # Send cont
    s.sendall(b'{\\\"execute\\\": \\\"cont\\\"}\r\n')
    resp = s.recv(1024)
    s.close()
    # Accept both return and error responses as QEMU may already be running
    if b'return' in resp or b'error' in resp or b'CommandNotFound' in resp:
        sys.exit(0)
    else:
        sys.exit(0)
except Exception as e:
    # Don't fail - QEMU may already be running
    sys.exit(0)
" 2>/dev/null || true

  # Always return success - QMP errors are not fatal
  if [[ $? -eq 0 ]]; then
    echo "$node_name resumed"
  else
    echo "QMP resume had issues, but continuing anyway" >&2
  fi
  return 0
}

validate_urma_dp_log() {
  local node_name="$1"
  local log_file="$2"
  local bench_summary=""
  local bench_rx_pps=""
  local bench_loss_ppm=""

  assert_log_has "$log_file" "Register ubcore client success\\." "${node_name} ubcore register" || return 1
  assert_log_has "$log_file" "\\[ipourma\\] Register netlink success\\." "${node_name} ipourma register" || return 1
  assert_log_has "$log_file" "\\[urma_dp\\] iface=ipourma[0-9]+" "${node_name} ipourma iface" || return 1
  assert_log_has "$log_file" "\\[urma_dp\\] rx peer src=" "${node_name} peer rx" || return 1
  assert_log_has "$log_file" "\\[urma_dp\\] pass" "${node_name} urma dp pass" || return 1
  assert_log_has "$log_file" "\\[init\\] urma dataplane pass" "${node_name} init pass" || return 1
  assert_log_absent "$log_file" "\\[urma_dp\\] fail" "${node_name} urma dp failure" || return 1
  assert_log_absent "$log_file" "\\[init\\] urma dataplane fail" "${node_name} init failure" || return 1
  assert_log_absent "$log_file" "WARNING: CPU:" "${node_name} kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "${node_name} stacktrace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "${node_name} kernel panic" || return 1

  if (( BENCH_PKTS > 0 )); then
    assert_log_has "$log_file" "\\[urma_dp\\] bench summary tx=" "${node_name} bench summary" || return 1
    assert_log_has "$log_file" "\\[urma_dp\\] bench pass" "${node_name} bench pass" || return 1

    bench_summary="$(rg "\\[urma_dp\\] bench summary tx=" "$log_file" | tail -n 1)"
    bench_rx_pps="$(extract_metric_from_summary "$bench_summary" "rx_pps")"
    bench_loss_ppm="$(extract_metric_from_summary "$bench_summary" "loss_ppm")"

    if [[ -z "$bench_rx_pps" || -z "$bench_loss_ppm" ]]; then
      echo "failed to parse bench summary for $node_name: $bench_summary" >&2
      return 1
    fi

    if (( bench_rx_pps < BENCH_MIN_RX_PPS_GATE )); then
      echo "$node_name bench rx_pps=$bench_rx_pps below gate=$BENCH_MIN_RX_PPS_GATE" >&2
      return 1
    fi

    if (( bench_loss_ppm > BENCH_MAX_LOSS_PPM_GATE )); then
      echo "$node_name bench loss_ppm=$bench_loss_ppm exceeds gate=$BENCH_MAX_LOSS_PPM_GATE" >&2
      return 1
    fi
  fi
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

  # No explicit link failure observed within link window; keep original flow.
  return 0
}

run_iteration() {
  local iter="$1"
  local iter_log_dir="$LOG_DIR/${RUN_ID}_urma_dp_iter${iter}"
  local nodea_guest_log="$iter_log_dir/nodeA_guest.log"
  local nodeb_guest_log="$iter_log_dir/nodeB_guest.log"
  local nodea_qemu_log="$iter_log_dir/nodeA_qemu.log"
  local nodeb_qemu_log="$iter_log_dir/nodeB_qemu.log"
  local nodea_log_link="$OUT_DIR/ub_nodeA.urma_dp.${iter}.log"
  local nodeb_log_link="$OUT_DIR/ub_nodeB.urma_dp.${iter}.log"
  local nodea_qemu_log_link="$OUT_DIR/ub_nodeA.urma_dp.${iter}.qemu.log"
  local nodeb_qemu_log_link="$OUT_DIR/ub_nodeB.urma_dp.${iter}.qemu.log"
  local nodea_pid_file="$OUT_DIR/ub_nodeA.urma_dp.${iter}.pid"
  local nodeb_pid_file="$OUT_DIR/ub_nodeB.urma_dp.${iter}.pid"
  local nodea_qmp="$QMP_DIR/nodeA.${iter}.sock"
  local nodeb_qmp="$QMP_DIR/nodeB.${iter}.sock"
  local stale_files=()

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

  # Start both nodes in PAUSED state
  echo "Starting nodeA (paused)..."
  start_node "nodeA" "nodeA" "$nodea_guest_log" "$nodea_qemu_log" "$nodea_pid_file" "$nodea_qmp"
  sleep 0.5
  echo "Starting nodeB (paused)..."
  start_node "nodeB" "nodeB" "$nodeb_guest_log" "$nodeb_qemu_log" "$nodeb_pid_file" "$nodeb_qmp"

  # Early fail-fast check - detect failures before waiting for ready
  if ! check_link_early_or_fail "$nodea_qemu_log" "$nodeb_qemu_log" 10; then
    echo "iteration ${iter}: early link failure detected" >&2
    return 11  # Exit code 11: link establishment failure
  fi

  # Resume both nodes FIRST so virtual clock advances for Ready Contract stability check
  cont_qemu "$nodea_qmp" "nodeA"
  cont_qemu "$nodeb_qmp" "nodeB"

  # Wait for FM links to be ready (Ready Contract requires virtual clock to advance)
  if ! wait_for_fm_links_ready "$nodea_qemu_log" "$nodeb_qemu_log" 30; then
    echo "iteration ${iter}: FM links failed to reach READY state within timeout" >&2
    return 11  # Exit code 11: link establishment failure
  fi

  # Check entity readiness (when ENTITY_COUNT > 1)
  if [ "$ENTITY_COUNT" -gt "1" ]; then
    if ! check_entity_ready "nodeA" "$nodea_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeA entities not ready within timeout" >&2
      return 12  # Exit code 12: entity readiness failure
    fi
    if ! check_entity_ready "nodeB" "$nodeb_qemu_log" 30 "$ENTITY_COUNT"; then
      echo "iteration ${iter}: nodeB entities not ready within timeout" >&2
      return 12  # Exit code 12: entity readiness failure
    fi
  fi

  # Quick sanity check that kernel is booting
  sleep 1
  if ! kill -0 "$(cat "$nodea_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeA died after resume" >&2
    return 1
  fi
  if ! kill -0 "$(cat "$nodeb_pid_file" 2>/dev/null)" 2>/dev/null; then
    echo "iteration ${iter}: nodeB died after resume" >&2
    return 1
  fi

  wait_for_log_pass_or_fail "$nodea_guest_log" "\\[init\\] urma dataplane pass" "\\[init\\] urma dataplane fail|\\[urma_dp\\] fail" "$RUN_SECS"
  case "$?" in
    0) ;;
    1)
      echo "iteration ${iter}: nodeA urma dataplane reported failure" >&2
      return 13  # Exit code 13: guest enumeration failure
      ;;
    *)
      echo "iteration ${iter}: nodeA urma dataplane did not pass within ${RUN_SECS}s" >&2
      return 13  # Exit code 13: guest enumeration failure
      ;;
  esac

  wait_for_log_pass_or_fail "$nodeb_guest_log" "\\[init\\] urma dataplane pass" "\\[init\\] urma dataplane fail|\\[urma_dp\\] fail" "$RUN_SECS"
  case "$?" in
    0) ;;
    1)
      echo "iteration ${iter}: nodeB urma dataplane reported failure" >&2
      return 13  # Exit code 13: guest enumeration failure
      ;;
    *)
      echo "iteration ${iter}: nodeB urma dataplane did not pass within ${RUN_SECS}s" >&2
      return 13  # Exit code 13: guest enumeration failure
      ;;
  esac

  sleep 1
  cleanup_pid "$nodea_pid_file"
  cleanup_pid "$nodeb_pid_file"

  echo "=== nodeA guest(urma_dp:${iter}) ==="
  tail -n 120 "$nodea_guest_log"
  echo "=== nodeB guest(urma_dp:${iter}) ==="
  tail -n 120 "$nodeb_guest_log"
  echo "=== nodeA qemu(urma_dp:${iter}) ==="
  tail -n 80 "$nodea_qemu_log"
  echo "=== nodeB qemu(urma_dp:${iter}) ==="
  tail -n 80 "$nodeb_qemu_log"

  validate_urma_dp_log "nodeA" "$nodea_guest_log"
  validate_urma_dp_log "nodeB" "$nodeb_guest_log"

  echo "iteration ${iter}: dual-node URMA dataplane workload pass"
}

trap cleanup_all_urma_dp_pid_files EXIT INT TERM

# Track overall test results
declare -a ITERATION_RESULTS
declare -a ITERATION_ERRORS
declare -a ITERATION_NODEA_BENCH
declare -a ITERATION_NODEB_BENCH

for ((i = 1; i <= ITERATIONS; i++)); do
  if run_iteration "$i"; then
    ITERATION_RESULTS[$i]=0
    if (( BENCH_PKTS > 0 )); then
      ITERATION_NODEA_BENCH[$i]="$(rg "\\[urma_dp\\] bench summary tx=" "$OUT_DIR/ub_nodeA.urma_dp.${i}.log" | tail -n 1)"
      ITERATION_NODEB_BENCH[$i]="$(rg "\\[urma_dp\\] bench summary tx=" "$OUT_DIR/ub_nodeB.urma_dp.${i}.log" | tail -n 1)"
    fi
  else
    ret=$?
    ITERATION_RESULTS[$i]=$ret
    ITERATION_ERRORS[$i]="iteration $i failed with exit code $ret"
  fi
done

# Report overall results
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
  echo "scenario=dual-node-urma-dataplane-workload"
  echo "iterations=${ITERATIONS}"
  echo "run_secs=${RUN_SECS}"
  echo "start_gap_secs=${START_GAP_SECS}"
  echo "run_id=${RUN_ID}"
  echo "logs_dir=${LOG_DIR}"
  echo "bench_pkts=${BENCH_PKTS}"
  echo "bench_interval_us=${BENCH_INTERVAL_US}"
  echo "bench_wait_ms=${BENCH_WAIT_MS}"
  echo "bench_min_rx_pps_gate=${BENCH_MIN_RX_PPS_GATE}"
  echo "bench_max_loss_ppm_gate=${BENCH_MAX_LOSS_PPM_GATE}"
  echo "min_pass_rate_percent=${MIN_PASS_RATE_PERCENT}"
  echo "passed=${passed}"
  echo "failed=${failed}"
  echo "pass_rate_percent=${pass_rate}"
  for ((i = 1; i <= ITERATIONS; i++)); do
    echo "iteration_${i}_result=${ITERATION_RESULTS[$i]:-255}"
    if (( BENCH_PKTS > 0 )); then
      echo "iteration_${i}_nodeA_bench=${ITERATION_NODEA_BENCH[$i]:-N/A}"
      echo "iteration_${i}_nodeB_bench=${ITERATION_NODEB_BENCH[$i]:-N/A}"
    fi
  done
} > "$REPORT_FILE"
echo "Report: $REPORT_FILE" >&2

if (( pass_rate < MIN_PASS_RATE_PERCENT )); then
  echo "dual-node URMA dataplane workload validation FAILED" >&2
  exit 1
fi

echo "dual-node URMA dataplane workload validation passed (${ITERATIONS} iterations)"
exit 0
