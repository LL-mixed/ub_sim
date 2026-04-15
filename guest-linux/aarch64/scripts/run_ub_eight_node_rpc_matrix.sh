#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_rpc_matrix.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_rpc_matrix.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_rpc8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
CLEANUP_SCRIPT="$OUT_DIR/headless_eight_node_cleanup.${RUN_ID_BASE}.sh"
ENV_FILE="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
SERVER_WAIT_SECS="${SERVER_WAIT_SECS:-60}"
CALL_WAIT_SECS="${CALL_WAIT_SECS:-120}"
START_GAP_SECS="${START_GAP_SECS:-1}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((59500 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
typeset -A SERVER_EXPECT_COUNTS
DEFAULT_CALL_LIST=(
  "nodeA nodeB" "nodeA nodeC" "nodeA nodeD" "nodeA nodeE" "nodeA nodeF" "nodeA nodeG" "nodeA nodeH"
  "nodeB nodeA" "nodeB nodeC" "nodeB nodeD" "nodeB nodeE" "nodeB nodeF" "nodeB nodeG" "nodeB nodeH"
  "nodeC nodeA" "nodeC nodeB" "nodeC nodeD" "nodeC nodeE" "nodeC nodeF" "nodeC nodeG" "nodeC nodeH"
  "nodeD nodeA" "nodeD nodeB" "nodeD nodeC" "nodeD nodeE" "nodeD nodeF" "nodeD nodeG" "nodeD nodeH"
  "nodeE nodeA" "nodeE nodeB" "nodeE nodeC" "nodeE nodeD" "nodeE nodeF" "nodeE nodeG" "nodeE nodeH"
  "nodeF nodeA" "nodeF nodeB" "nodeF nodeC" "nodeF nodeD" "nodeF nodeE" "nodeF nodeG" "nodeF nodeH"
  "nodeG nodeA" "nodeG nodeB" "nodeG nodeC" "nodeG nodeD" "nodeG nodeE" "nodeG nodeF" "nodeG nodeH"
  "nodeH nodeA" "nodeH nodeB" "nodeH nodeC" "nodeH nodeD" "nodeH nodeE" "nodeH nodeF" "nodeH nodeG"
)
CALL_LIST=()
if [[ -n "${CALL_LIST_OVERRIDE:-}" ]]; then
  while IFS= read -r call; do
    [[ -n "$call" ]] && CALL_LIST+=("$call")
  done <<EOF_CALLS
${CALL_LIST_OVERRIDE}
EOF_CALLS
else
  CALL_LIST=("${DEFAULT_CALL_LIST[@]}")
fi

for node_id in "${NODE_IDS[@]}"; do
  SERVER_EXPECT_COUNTS[$node_id]=0
done
for call in "${CALL_LIST[@]}"; do
  dst="${call##* }"
  SERVER_EXPECT_COUNTS[$dst]=$(( SERVER_EXPECT_COUNTS[$dst] + 2 ))
done

trace() {
  local msg="$1"
  printf '[rpc8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

wait_for_log_pattern_since() {
  local file="$1"
  local start_line="$2"
  local pattern="$3"
  local timeout_s="$4"
  local deadline=$((SECONDS + timeout_s))
  local tmp

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$pattern"; then
        return 0
      fi
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

slice_log_since() {
  local src="$1"
  local start_line="$2"
  local dst="$3"
  if [[ ! -f "$src" ]]; then
    : > "$dst"
    return 0
  fi
  tail -n "+$((start_line + 1))" "$src" > "$dst"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

send_rpc_server_cmd() {
  local local_ip="$1"
  local serial_port="$2"
  local start_marker="$3"
  local expected_calls="$4"
  local payload

  payload=$'export LINQU_RPC_MODE=server\n'
  payload+=$'export LINQU_RPC_EXPECT_CALLS='"${expected_calls}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_rpc &\n'

  send_serial_block "$serial_port" "$payload"
}

send_rpc_client_cmd() {
  local local_ip="$1"
  local peer_ip="$2"
  local serial_port="$3"
  local start_marker="$4"
  local payload

  payload=$'export LINQU_RPC_MODE=client\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_PEER_IP='"${peer_ip}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_rpc\n'

  send_serial_block "$serial_port" "$payload"
}

capture_guest_diag() {
  local node_id="$1"
  local serial_port="$2"
  send_serial_block "$serial_port" $'echo DIAG_'"${node_id}"$'_START\nifconfig ipourma0\nps\n'
}

prepare_single_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  ENV_FILE="$ENV_FILE" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" \
    UB_SIM_PORT_NUM=7 "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null
  source "$ENV_FILE"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "prepare: wait shell gate $node_id"
    if ! wait_for_guest_shell "$guest_log" "$BOOT_WAIT_SECS"; then
      trace "prepare: shell gate timeout $node_id"
      echo "$node_id did not finish shell boot gate" >&2
      cleanup_headless_env "$CLEANUP_SCRIPT"
      return 1
    fi
    trace "prepare: shell gate ok $node_id"
  done

  sleep 2
  trace "prepare: environment ready"
  return 0
}

start_all_servers() {
  local node_id node_ip_addr serial_port guest_log start_marker expected_calls

  for node_id in "${NODE_IDS[@]}"; do
    expected_calls="${SERVER_EXPECT_COUNTS[$node_id]}"
    if (( expected_calls == 0 )); then
      trace "server ${node_id}: skipped expected_calls=0"
      continue
    fi
    node_ip_addr="$(node_ip "$node_id")"
    serial_port="$(node_serial_port "$node_id" "$PORT_BASE")"
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_marker="RPC_SERVER_START_${node_id}"
    trace "server ${node_id}: launch expected_calls=${expected_calls}"
    send_rpc_server_cmd "$node_ip_addr" "$serial_port" "$start_marker" "$expected_calls"
    if ! wait_for_log_pattern "$guest_log" "$start_marker" 20; then
      echo "server start marker missing for $node_id" >&2
      return 1
    fi
    if ! wait_for_log_pattern "$guest_log" "\\[ub_rpc\\] mode=server local=${node_ip_addr} peer=<dynamic> expected_calls=${expected_calls} started" "$SERVER_WAIT_SECS"; then
      echo "server did not start on $node_id" >&2
      return 1
    fi
    trace "server ${node_id}: ready"
    sleep "$START_GAP_SECS"
  done
}

validate_client_slice() {
  local src="$1"
  local dst="$2"
  local src_ip="$3"
  local dst_ip="$4"
  local log_file="$5"

  assert_log_has "$log_file" "\\[ub_rpc\\] pass" "$src->$dst client pass" || return 1
  assert_log_absent "$log_file" "\\[ub_rpc\\] fail" "$src->$dst client fail" || return 1
  assert_log_has "$log_file" "\\[RPC\\] client local=${src_ip//./\\.} peer=${dst_ip//./\\.} op=ECHO msg_id=1 status=OK result=\"greeting from rpc client ${src_ip//./\\.}\" expected=\"greeting from rpc client ${src_ip//./\\.}\" verified=1" \
    "$src->$dst client echo verified" || return 1
  assert_log_has "$log_file" "\\[RPC\\] client local=${src_ip//./\\.} peer=${dst_ip//./\\.} op=CRC32 msg_id=2 status=OK payload=\"rpc crc payload from ${src_ip//./\\.} to ${dst_ip//./\\.} over ub_link\" result=\"0x[0-9a-f]{8}\" expected=\"0x[0-9a-f]{8}\" verified=1" \
    "$src->$dst client crc verified" || return 1
}

validate_server_slice() {
  local src="$1"
  local dst="$2"
  local src_ip="$3"
  local dst_ip="$4"
  local log_file="$5"

  assert_log_has "$log_file" "\\[RPC\\] server local=${dst_ip//./\\.} peer=${src_ip//./\\.} handled op=ECHO msg_id=1 rpc_count=[0-9]+" \
    "$src->$dst server echo handled" || return 1
  assert_log_has "$log_file" "\\[RPC\\] server local=${dst_ip//./\\.} peer=${src_ip//./\\.} handled op=CRC32 msg_id=2 rpc_count=[0-9]+" \
    "$src->$dst server crc handled" || return 1
}

run_directed_call() {
  local call_idx="$1"
  local src="$2"
  local dst="$3"
  local src_ip dst_ip src_serial src_log dst_log
  local src_start_line=0 dst_start_line=0
  local src_marker client_slice server_slice rc=0

  src_ip="$(node_ip "$src")"
  dst_ip="$(node_ip "$dst")"
  src_serial="$(node_serial_port "$src" "$PORT_BASE")"
  src_log="$RUN_DIR/${src}_guest.log"
  dst_log="$RUN_DIR/${dst}_guest.log"
  src_marker="RPC_CLIENT_START_${call_idx}_${src}_${dst}"
  client_slice="$OUT_DIR/eight_node_rpc_${src}_${dst}_client.slice.log"
  server_slice="$OUT_DIR/eight_node_rpc_${src}_${dst}_server.slice.log"

  [[ -f "$src_log" ]] && src_start_line="$(wc -l < "$src_log")"
  [[ -f "$dst_log" ]] && dst_start_line="$(wc -l < "$dst_log")"

  trace "call ${src}->${dst}: launch client"
  if ! send_rpc_client_cmd "$src_ip" "$dst_ip" "$src_serial" "$src_marker"; then
    echo "$src->$dst: failed to send client command" >&2
    return 1
  fi
  if ! wait_for_log_pattern "$src_log" "$src_marker" 20; then
    echo "$src->$dst: missing client start marker" >&2
    return 1
  fi

  rc=0
  wait_for_log_pass_or_fail_since "$src_log" "$src_start_line" "\\[ub_rpc\\] pass" "\\[ub_rpc\\] fail" "$CALL_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    capture_guest_diag "$src" "$src_serial"
    echo "$src->$dst: client did not pass" >&2
    return 1
  fi

  if ! wait_for_log_pattern_since "$dst_log" "$dst_start_line" "\\[RPC\\] server local=${dst_ip//./\\.} peer=${src_ip//./\\.} handled op=CRC32 msg_id=2 rpc_count=[0-9]+" "$CALL_WAIT_SECS"; then
    echo "$src->$dst: server did not handle rpc in time" >&2
    return 1
  fi

  slice_log_since "$src_log" "$src_start_line" "$client_slice"
  slice_log_since "$dst_log" "$dst_start_line" "$server_slice"

  validate_client_slice "$src" "$dst" "$src_ip" "$dst_ip" "$client_slice" || return 1
  validate_server_slice "$src" "$dst" "$src_ip" "$dst_ip" "$server_slice" || return 1
  trace "call ${src}->${dst}: pass"
}

wait_for_servers_to_finish() {
  local node_id node_ip_addr guest_log expected_calls

  for node_id in "${NODE_IDS[@]}"; do
    expected_calls="${SERVER_EXPECT_COUNTS[$node_id]}"
    if (( expected_calls == 0 )); then
      continue
    fi
    node_ip_addr="$(node_ip "$node_id")"
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "server ${node_id}: wait exit"
    if ! wait_for_log_pattern "$guest_log" "\\[ub_rpc\\] mode=server local=${node_ip_addr} exiting handled=${expected_calls}" "$CALL_WAIT_SECS"; then
      echo "server $node_id did not exit cleanly" >&2
      return 1
    fi
    assert_log_has "$guest_log" "\\[ub_rpc\\] pass" "${node_id} server pass" || return 1
    assert_log_absent "$guest_log" "\\[ub_rpc\\] fail" "${node_id} server fail" || return 1
  done
}

chmod +x "$0" 2>/dev/null || true
mkdir -p "$OUT_DIR"
{
  echo "eight-node rpc matrix"
  echo "run_id_base=$RUN_ID_BASE"
  echo "run_dir=$RUN_DIR"
  echo "boot_wait_secs=$BOOT_WAIT_SECS"
  echo "server_wait_secs=$SERVER_WAIT_SECS"
  echo "call_wait_secs=$CALL_WAIT_SECS"
  echo "start_gap_secs=$START_GAP_SECS"
  echo
} > "$REPORT_FILE"

if ! prepare_single_environment; then
  exit 1
fi

if ! start_all_servers; then
  cleanup_headless_env "$CLEANUP_SCRIPT"
  exit 1
fi

call_idx=1
for call in "${CALL_LIST[@]}"; do
  src="${call%% *}"
  dst="${call##* }"
  if run_directed_call "$call_idx" "$src" "$dst"; then
    echo "$src->$dst: PASS" | tee -a "$REPORT_FILE"
  else
    echo "$src->$dst: FAIL" | tee -a "$REPORT_FILE"
    cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi
  call_idx=$((call_idx + 1))
  echo >> "$REPORT_FILE"
done

if ! wait_for_servers_to_finish; then
  cleanup_headless_env "$CLEANUP_SCRIPT"
  exit 1
fi

cleanup_headless_env "$CLEANUP_SCRIPT"
echo "result=PASS" | tee -a "$REPORT_FILE"
