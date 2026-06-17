#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_obmm_queue.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_obmm_queue.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmm_queue8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
APP_WAIT_SECS="${APP_WAIT_SECS:-${DEMO_WAIT_SECS:-300}}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
PORT_BASE_START="${PORT_BASE_START:-$((53600 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
OBMM_POOL_EXPORT_SIZE_MB="${OBMM_POOL_EXPORT_SIZE_MB:-512}"
OBMM_QUEUE_DEPTH="${OBMM_QUEUE_DEPTH:-1024}"
OBMM_BOOTSTRAP="${OBMM_BOOTSTRAP:-fm}"
OBMM_QUEUE_MODE="${OBMM_QUEUE_MODE:-${OBMM_DEMO_MODE:-combined}}"
OBMM_SPMC_PROVIDER="${OBMM_SPMC_PROVIDER:-0}"
OBMM_SPMC_BATCH_COUNT="${OBMM_SPMC_BATCH_COUNT:-1000}"
OBMM_MPSC_CONSUMER="${OBMM_MPSC_CONSUMER:-0}"
OBMM_MPSC_BATCH_COUNT="${OBMM_MPSC_BATCH_COUNT:-1000}"
OBMM_MPMC_BATCH_COUNT="${OBMM_MPMC_BATCH_COUNT:-500}"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
ALL_IPS_CSV="${(j:,:)NODE_IPS}"

trace() {
  local msg="$1"
  printf '[obmm-queue8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

wait_for_log_pass_or_fail_since() {
  local file="$1"
  local start_line="$2"
  local pass_pattern="$3"
  local fail_pattern="$4"
  local timeout_s="$5"
  local deadline=$((SECONDS + timeout_s))
  local tmp

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$pass_pattern"; then
        return 0
      fi
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$fail_pattern"; then
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

node_index() {
  case "$1" in
    nodeA) echo 1 ;;
    nodeB) echo 2 ;;
    nodeC) echo 3 ;;
    nodeD) echo 4 ;;
    nodeE) echo 5 ;;
    nodeF) echo 6 ;;
    nodeG) echo 7 ;;
    nodeH) echo 8 ;;
    *) return 1 ;;
  esac
}

node_ip() {
  local idx="$(node_index "$1")"
  echo "${NODE_IPS[$idx]}"
}

node_serial_port() {
  local node_id="$1"
  local port_base="$2"
  local idx="$(node_index "$node_id")"
  echo $((port_base + 31 + idx))
}

node_serial_endpoint() {
  local node_id="$1"
  local port_base="$2"
  case "$node_id" in
    nodeA) echo "${NODEA_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeB) echo "${NODEB_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeC) echo "${NODEC_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeD) echo "${NODED_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeE) echo "${NODEE_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeF) echo "${NODEF_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeG) echo "${NODEG_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    nodeH) echo "${NODEH_SERIAL_SOCKET:-$(node_serial_port "$node_id" "$port_base")}" ;;
    *) return 1 ;;
  esac
}

send_serial_block() {
  local endpoint="$1"
  local payload="$2"
  python3 - "$endpoint" "$payload" <<'PY'
import socket
import sys
import time
endpoint = sys.argv[1]
payload = sys.argv[2]
deadline = time.time() + 20.0
last_err = None
while time.time() < deadline:
    if endpoint.startswith("/"):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        connect_arg = endpoint
    else:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        connect_arg = ("127.0.0.1", int(endpoint))
    s.settimeout(5)
    try:
        s.connect(connect_arg)
        s.sendall(payload.encode("utf-8"))
        time.sleep(0.2)
        s.close()
        sys.exit(0)
    except OSError as exc:
        last_err = exc
        try:
            s.close()
        except OSError:
            pass
        time.sleep(0.5)
raise last_err if last_err is not None else TimeoutError("serial connect timeout")
PY
}

wait_for_guest_shell() {
  local file="$1"
  local timeout_s="$2"
  wait_for_log_pattern "$file" "\\[run_demo\\] boot flow completed, dropping to shell" "$timeout_s"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

send_obmm_queue_cmd() {
  local local_ip="$1"
  local serial_endpoint="$2"
  local start_marker="$3"
  local payload

  payload=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_ALL_IPS='"${ALL_IPS_CSV}"$'\n'
  payload+=$'export LINQU_UB_NODE_COUNT=8\n'
  payload+=$'export OBMM_POOL_EXPORT_SIZE_MB='"${OBMM_POOL_EXPORT_SIZE_MB}"$'\n'
  payload+=$'export OBMM_QUEUE_DEPTH='"${OBMM_QUEUE_DEPTH}"$'\n'
  payload+=$'export OBMM_BOOTSTRAP='"${OBMM_BOOTSTRAP}"$'\n'
  payload+=$'export OBMM_BOOTSTRAP_SESSION='"${RUN_ID_BASE}"$'\n'
  payload+=$'export OBMM_QUEUE_MODE='"${OBMM_QUEUE_MODE}"$'\n'
  payload+=$'export OBMM_SPMC_PROVIDER='"${OBMM_SPMC_PROVIDER}"$'\n'
  payload+=$'export OBMM_SPMC_BATCH_COUNT='"${OBMM_SPMC_BATCH_COUNT}"$'\n'
  payload+=$'export OBMM_MPSC_CONSUMER='"${OBMM_MPSC_CONSUMER}"$'\n'
  payload+=$'export OBMM_MPSC_BATCH_COUNT='"${OBMM_MPSC_BATCH_COUNT}"$'\n'
  payload+=$'export OBMM_MPMC_BATCH_COUNT='"${OBMM_MPMC_BATCH_COUNT}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_obmm_queue\n'

  send_serial_block "$serial_endpoint" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP export_size_mb=$OBMM_POOL_EXPORT_SIZE_MB queue_depth=$OBMM_QUEUE_DEPTH bootstrap=$OBMM_BOOTSTRAP"
  if ! ENV_FILE="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" QEMU_MEM="$QEMU_MEM" QEMU_SMP="$QEMU_SMP" APPEND_EXTRA="$APPEND_BASE" \
    "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null; then
    trace "FAIL: launch headless env failed"
    return 1
  fi
  if [[ ! -f "$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh" ]]; then
    trace "FAIL: missing headless env file"
    return 1
  fi
  source "$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "wait shell gate: $node_id"
    if ! wait_for_guest_shell "$guest_log" "$BOOT_WAIT_SECS"; then
      trace "FAIL: shell gate timeout for $node_id"
      return 1
    fi
  done
  trace "shell gate ok for all eight nodes"
  return 0
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local owner_idx

  owner_idx="$(node_index "$node_id")"

  assert_log_has "$log_file" "\\[obmm_queue\\] export -> ok" \
    "$node_id export" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] export layout -> ok" \
    "$node_id layout" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] bootstrap ${OBMM_BOOTSTRAP} -> ok count=8" \
    "$node_id bootstrap exchange" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] pool ready -> ok nodes=8" \
    "$node_id pool ready" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] rounds -> ok count=8" \
    "$node_id rounds done" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] queue stress -> ok passes=2 depth=${OBMM_QUEUE_DEPTH}" \
    "$node_id queue stress" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] spmc.*-> ok" \
    "$node_id spmc" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] mpsc.*-> ok" \
    "$node_id mpsc" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] mpmc -> ok" \
    "$node_id mpmc" || return 1
  assert_log_has "$log_file" "\\[obmm_queue\\] pass" "$node_id pass" || return 1
  assert_log_absent "$log_file" "\\[obmm_queue\\] .*fail" "$node_id fail" || return 1
  assert_log_absent "$log_file" "WARNING: CPU:" "$node_id kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "$node_id call trace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "$node_id kernel panic" || return 1
}

run_queue_app() {
  local node_id
  local guest_log
  local serial_endpoint
  local start_marker
  local start_line
  local rc
  typeset -A START_LINES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_line=$(wc -l < "$guest_log" | tr -d ' ')
    START_LINES[$node_id]="$start_line"
    serial_endpoint="$(node_serial_endpoint "$node_id" "$PORT_BASE")"
    start_marker="OBMM_QUEUE_${node_id}_START"
    trace "start queue app on $node_id serial=$serial_endpoint"
    send_obmm_queue_cmd "$(node_ip "$node_id")" "$serial_endpoint" "$start_marker"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    rc=0
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[obmm_queue\\] pass" "\\[obmm_queue\\] .*fail" "$APP_WAIT_SECS" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      trace "FAIL: queue app did not pass on $node_id rc=$rc"
      return 1
    fi
  done

  for node_id in "${NODE_IDS[@]}"; do
    validate_node_log "$node_id" "$RUN_DIR/${node_id}_guest.log" || return 1
  done
  return 0
}

main() {
  local cleanup_script=""
  local result="FAIL"

  mkdir -p "$OUT_DIR" "$LOG_DIR"
  : > "$REPORT_FILE"
  : > "$TRACE_FILE"

  if prepare_environment; then
    cleanup_script="$CLEANUP_SCRIPT"
    if run_queue_app; then
      result="PASS"
    fi
  fi

  {
    echo "run_id=$RUN_ID_BASE"
    echo "result=$result"
    echo "run_dir=$RUN_DIR"
    [[ -n "$cleanup_script" ]] && echo "cleanup_script=$cleanup_script"
  } | tee "$REPORT_FILE"

  if [[ -n "$cleanup_script" ]]; then
    cleanup_headless_env "$cleanup_script"
  fi

  [[ "$result" == "PASS" ]]
}

main "$@"
