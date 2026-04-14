#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/four_node_obmm_pool.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/four_node_obmm_pool.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmmpool4_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless4"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
DEMO_WAIT_SECS="${DEMO_WAIT_SECS:-180}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((53600 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"

NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)
ALL_IPS_CSV="${(j:,:)NODE_IPS}"

trace() {
  local msg="$1"
  printf '[obmmpool4] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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
  echo $((port_base + 15 + idx))
}

send_serial_block() {
  local port="$1"
  local payload="$2"
  python3 - "$port" "$payload" <<'PY'
import socket
import sys
import time
port = int(sys.argv[1])
payload = sys.argv[2]
deadline = time.time() + 20.0
last_err = None
while time.time() < deadline:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(payload.encode("utf-8"))
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

send_obmm_pool_cmd() {
  local local_ip="$1"
  local serial_port="$2"
  local start_marker="$3"
  local payload

  payload=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_ALL_IPS='"${ALL_IPS_CSV}"$'\n'
  payload+=$'export LINQU_UB_NODE_COUNT=4\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_obmm_demo\n'

  send_serial_block "$serial_port" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  ENV_FILE="$OUT_DIR/headless_four_node_env.${RUN_ID_BASE}.sh" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" \
    "$SCRIPT_DIR/launch_ub_four_node_headless.sh" >/dev/null
  source "$OUT_DIR/headless_four_node_env.${RUN_ID_BASE}.sh"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "wait shell gate: $node_id"
    if ! wait_for_guest_shell "$guest_log" "$BOOT_WAIT_SECS"; then
      trace "FAIL: shell gate timeout for $node_id"
      return 1
    fi
  done
  trace "shell gate ok for all four nodes"
  return 0
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local owner_idx

  owner_idx="$(node_index "$node_id")"

  assert_log_has "$log_file" "\\[ub_obmm_pool\\] export -> ok mem_id=[0-9]+ uba=0x[0-9a-f]+ token=[0-9]+" \
    "$node_id export" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] metadata exchange -> ok count=4" \
    "$node_id metadata exchange" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] import_all -> ok remote_slots=3" \
    "$node_id import all" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool ready -> ok nodes=4" \
    "$node_id pool ready" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round owner=${owner_idx} write_local -> ok slot=${owner_idx}" \
    "$node_id local round write" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=1 -> ok slot=1" \
    "$node_id round1 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=2 -> ok slot=2" \
    "$node_id round2 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=3 -> ok slot=3" \
    "$node_id round3 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] round verify owner=4 -> ok slot=4" \
    "$node_id round4 verify" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pool rounds -> ok count=4" \
    "$node_id rounds done" || return 1
  assert_log_has "$log_file" "\\[ub_obmm_pool\\] pass" "$node_id pass" || return 1
  assert_log_absent "$log_file" "\\[ub_obmm_pool\\] fail" "$node_id fail" || return 1
  assert_log_absent "$log_file" "WARNING: CPU:" "$node_id kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "$node_id call trace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "$node_id kernel panic" || return 1
}

run_pool_demo() {
  local node_id
  local guest_log
  local serial_port
  local start_marker
  local start_line
  local rc
  typeset -A START_LINES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_line=$(wc -l < "$guest_log" | tr -d ' ')
    START_LINES[$node_id]="$start_line"
    serial_port="$(node_serial_port "$node_id" "$PORT_BASE")"
    start_marker="OBMM_POOL_${node_id}_START"
    trace "start pool demo on $node_id serial=$serial_port"
    send_obmm_pool_cmd "$(node_ip "$node_id")" "$serial_port" "$start_marker"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    rc=0
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[ub_obmm_pool\\] pass" "\\[ub_obmm_pool\\] fail" "$DEMO_WAIT_SECS" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      trace "FAIL: pool demo did not pass on $node_id rc=$rc"
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
    if run_pool_demo; then
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
