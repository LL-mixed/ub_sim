#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_tcp_each_server_matrix.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_tcp_each_server_matrix.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_tcp_each_server8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
PAIR_WAIT_SECS="${PAIR_WAIT_SECS:-90}"
START_GAP_SECS="${START_GAP_SECS:-0}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
TCP_BENCHMARK="${TCP_BENCHMARK:-0}"
TCP_BENCH_SIZE="${TCP_BENCH_SIZE:-2097152}"
TCP_BENCH_ITERATIONS="${TCP_BENCH_ITERATIONS:-8192}"
TCP_BENCH_CHUNK_SIZE="${TCP_BENCH_CHUNK_SIZE:-64}"
TCP_BENCH_VERIFY="${TCP_BENCH_VERIFY:-0}"
TCP_BENCH_ONE_WAY="${TCP_BENCH_ONE_WAY:-0}"
TCP_BENCH_PROGRESS_INTERVAL="${TCP_BENCH_PROGRESS_INTERVAL:-64}"
PORT_BASE_START="${PORT_BASE_START:-$((53600 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
DEFAULT_PAIR_LIST=(
  "nodeA nodeB" "nodeA nodeC" "nodeA nodeD" "nodeA nodeE" "nodeA nodeF" "nodeA nodeG" "nodeA nodeH"
  "nodeB nodeC" "nodeB nodeD" "nodeB nodeE" "nodeB nodeF" "nodeB nodeG" "nodeB nodeH"
  "nodeC nodeD" "nodeC nodeE" "nodeC nodeF" "nodeC nodeG" "nodeC nodeH"
  "nodeD nodeE" "nodeD nodeF" "nodeD nodeG" "nodeD nodeH"
  "nodeE nodeF" "nodeE nodeG" "nodeE nodeH"
  "nodeF nodeG" "nodeF nodeH"
  "nodeG nodeH"
)
PAIR_LIST=()
if [[ -n "${PAIR_LIST_OVERRIDE:-}" ]]; then
  while IFS= read -r pair; do
    [[ -n "$pair" ]] && PAIR_LIST+=("$pair")
  done <<EOF_PAIRS
${PAIR_LIST_OVERRIDE}
EOF_PAIRS
else
  PAIR_LIST=("${DEFAULT_PAIR_LIST[@]}")
fi

trace() {
  local msg="$1"
  printf '[tcp-each-server8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

assert_log_has_since() {
  local file="$1"
  local start_line="$2"
  local pattern="$3"
  local label="$4"
  local tmp

  tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
  if [[ -z "$tmp" ]] || ! printf '%s\n' "$tmp" | rg -q "$pattern"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_absent_since() {
  local file="$1"
  local start_line="$2"
  local pattern="$3"
  local label="$4"
  local tmp

  tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
  if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$pattern"; then
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
  wait_for_log_pattern "$file" "\\[run_app\\] entering interactive shell" "$timeout_s"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

send_tcp_cmd() {
  local local_ip="$1"
  local peer_ip="$2"
  local role="$3"
  local serial_endpoint="$4"
  local start_marker="$5"
  local payload

  payload=$'export LINQU_URMA_DP_ROLE='"${role}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_PEER_IP='"${peer_ip}"$'\n'
  payload+=$'export TCP_BENCHMARK='"${TCP_BENCHMARK}"$'\n'
  payload+=$'export TCP_BENCH_SIZE='"${TCP_BENCH_SIZE}"$'\n'
  payload+=$'export TCP_BENCH_ITERATIONS='"${TCP_BENCH_ITERATIONS}"$'\n'
  payload+=$'export TCP_BENCH_CHUNK_SIZE='"${TCP_BENCH_CHUNK_SIZE}"$'\n'
  payload+=$'export TCP_BENCH_VERIFY='"${TCP_BENCH_VERIFY}"$'\n'
  payload+=$'export TCP_BENCH_ONE_WAY='"${TCP_BENCH_ONE_WAY}"$'\n'
  payload+=$'export TCP_BENCH_PROGRESS_INTERVAL='"${TCP_BENCH_PROGRESS_INTERVAL}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_tcp_each_server\n'

  send_serial_block "$serial_endpoint" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP pair_count=${#PAIR_LIST[@]}"
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

validate_pair_node_log() {
  local node_id="$1"
  local log_file="$2"
  local start_line="$3"
  local role="$4"
  local local_ip="$5"
  local peer_ip="$6"
  local peer_role

  if [[ "$role" == "nodeA" ]]; then
    peer_role="nodeB"
  else
    peer_role="nodeA"
  fi

  assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] start role=${role} .* local=${local_ip} peer=${peer_ip} port=18620" \
    "$node_id start" || return 1
  if [[ "$TCP_BENCHMARK" == "1" ]]; then
    if [[ "$TCP_BENCH_ONE_WAY" == "1" && "$role" == "nodeA" ]]; then
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_server_accepted role=${role}" \
        "$node_id benchmark server accepted" || return 1
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_server role=${role} iterations=${TCP_BENCH_ITERATIONS} .*verify_failures=0" \
        "$node_id benchmark server" || return 1
    elif [[ "$TCP_BENCH_ONE_WAY" == "1" && "$role" == "nodeB" ]]; then
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_client_connected role=${role}" \
        "$node_id benchmark client connected" || return 1
      assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} benchmark client complete iterations=${TCP_BENCH_ITERATIONS}" \
        "$node_id benchmark client complete" || return 1
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_result=done role=${role} .*verify_failures=0" \
        "$node_id benchmark result" || return 1
    else
      assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} benchmark client complete iterations=${TCP_BENCH_ITERATIONS}" \
        "$node_id benchmark client complete" || return 1
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_server role=${role} iterations=${TCP_BENCH_ITERATIONS} .*verify_failures=0" \
        "$node_id benchmark server" || return 1
      assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] benchmark_result=done role=${role} .*verify_failures=0" \
        "$node_id benchmark result" || return 1
    fi
  else
    assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} client sent=\"tcp hello from ${role} client\"" \
      "$node_id client send" || return 1
    assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} server received=\"tcp hello from ${peer_role} client\"" \
      "$node_id server receive" || return 1
    assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} server ack=\"tcp ack from ${role} server\"" \
      "$node_id server ack" || return 1
    assert_log_has_since "$log_file" "$start_line" "\\[TCP_EACH_SERVER\\] ${role} client received_ack=\"tcp ack from ${peer_role} server\"" \
      "$node_id client ack" || return 1
  fi
  assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] summary role=${role} local=${local_ip} peer=${peer_ip} port=18620" \
    "$node_id summary" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] pass" \
    "$node_id pass" || return 1
  assert_log_absent_since "$log_file" "$start_line" "\\[ub_tcp_each_server\\] fail" "$node_id fail" || return 1
  assert_log_absent_since "$log_file" "$start_line" "Kernel panic - not syncing" "$node_id kernel panic" || return 1
}

run_pair() {
  local left_node="$1"
  local right_node="$2"
  local left_log="$RUN_DIR/${left_node}_guest.log"
  local right_log="$RUN_DIR/${right_node}_guest.log"
  local left_start
  local right_start
  local left_ip
  local right_ip
  local left_serial
  local right_serial
  local rc

  left_start=$(wc -l < "$left_log" | tr -d ' ')
  right_start=$(wc -l < "$right_log" | tr -d ' ')
  left_ip="$(node_ip "$left_node")"
  right_ip="$(node_ip "$right_node")"
  left_serial="$(node_serial_endpoint "$left_node" "$PORT_BASE")"
  right_serial="$(node_serial_endpoint "$right_node" "$PORT_BASE")"

  trace "start pair ${left_node}<->${right_node} left_ip=$left_ip right_ip=$right_ip"
  send_tcp_cmd "$left_ip" "$right_ip" "nodeA" "$left_serial" "TCP_EACH_SERVER_${left_node}_${right_node}_START"
  sleep "$START_GAP_SECS"
  send_tcp_cmd "$right_ip" "$left_ip" "nodeB" "$right_serial" "TCP_EACH_SERVER_${right_node}_${left_node}_START"

  rc=0
  wait_for_log_pass_or_fail_since "$left_log" "$left_start" \
    "\\[ub_tcp_each_server\\] pass" "\\[ub_tcp_each_server\\] fail|Kernel panic - not syncing" \
    "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    trace "FAIL: left node did not pass pair ${left_node}<->${right_node} rc=$rc"
    return 1
  fi
  rc=0
  wait_for_log_pass_or_fail_since "$right_log" "$right_start" \
    "\\[ub_tcp_each_server\\] pass" "\\[ub_tcp_each_server\\] fail|Kernel panic - not syncing" \
    "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    trace "FAIL: right node did not pass pair ${left_node}<->${right_node} rc=$rc"
    return 1
  fi

  validate_pair_node_log "$left_node" "$left_log" "$left_start" "nodeA" "$left_ip" "$right_ip" || return 1
  validate_pair_node_log "$right_node" "$right_log" "$right_start" "nodeB" "$right_ip" "$left_ip" || return 1
}

run_matrix() {
  local pair
  local left_node
  local right_node

  for pair in "${PAIR_LIST[@]}"; do
    left_node="${pair%% *}"
    right_node="${pair##* }"
    if [[ "$left_node" == "$right_node" ]]; then
      trace "FAIL: invalid self pair $pair"
      return 1
    fi
    run_pair "$left_node" "$right_node" || return 1
  done
}

main() {
  local cleanup_script=""
  local result="FAIL"

  mkdir -p "$OUT_DIR" "$LOG_DIR"
  : > "$REPORT_FILE"
  : > "$TRACE_FILE"

  if prepare_environment; then
    cleanup_script="$CLEANUP_SCRIPT"
    if run_matrix; then
      result="PASS"
    fi
  fi

  {
    echo "run_id=$RUN_ID_BASE"
    echo "result=$result"
    echo "run_dir=$RUN_DIR"
    echo "pair_count=${#PAIR_LIST[@]}"
    echo "tcp_benchmark=$TCP_BENCHMARK"
    echo "tcp_bench_size=$TCP_BENCH_SIZE"
    echo "tcp_bench_iterations=$TCP_BENCH_ITERATIONS"
    echo "tcp_bench_chunk_size=$TCP_BENCH_CHUNK_SIZE"
    echo "tcp_bench_one_way=$TCP_BENCH_ONE_WAY"
    echo "tcp_bench_progress_interval=$TCP_BENCH_PROGRESS_INTERVAL"
    [[ -n "$cleanup_script" ]] && echo "cleanup_script=$cleanup_script"
  } | tee "$REPORT_FILE"

  if [[ -n "$cleanup_script" ]]; then
    cleanup_headless_env "$cleanup_script"
  fi

  [[ "$result" == "PASS" ]]
}

main "$@"
