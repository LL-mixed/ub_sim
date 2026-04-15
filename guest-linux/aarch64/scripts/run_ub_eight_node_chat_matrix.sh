#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_chat_matrix.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_chat_matrix.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_chat8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
CLEANUP_SCRIPT="$OUT_DIR/headless_eight_node_cleanup.${RUN_ID_BASE}.sh"
ENV_FILE="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
PAIR_WAIT_SECS="${PAIR_WAIT_SECS:-180}"
START_GAP_SECS="${START_GAP_SECS:-1}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((59000 + (RANDOM % 300)))}"
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
  printf '[chat8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

send_serial_line() {
  local port="$1"
  local line="$2"
  python3 - "$port" "$line" <<'PY'
import socket
import sys
import time
port = int(sys.argv[1])
line = sys.argv[2]
deadline = time.time() + 20.0
last_err = None
while time.time() < deadline:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(line.encode("utf-8") + b"\n")
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

validate_chat_logs() {
  local initiator="$1"
  local responder="$2"
  local initiator_log="$3"
  local responder_log="$4"
  local pair_label="$initiator-$responder"

  assert_log_has "$initiator_log" "\\[ub_chat\\] pass" "$pair_label initiator pass" || return 1
  assert_log_absent "$initiator_log" "\\[ub_chat\\] fail" "$pair_label initiator fail" || return 1
  assert_log_has "$initiator_log" "\\[ub_chat\\] summary tx=5 rx=5" "$pair_label initiator summary" || return 1
  assert_log_has "$initiator_log" "\\[CHAT\\] initiator seq=[0-9]+ \"copy, greeting back from responder\"" "$pair_label initiator payload" || return 1

  assert_log_has "$responder_log" "\\[ub_chat\\] pass" "$pair_label responder pass" || return 1
  assert_log_absent "$responder_log" "\\[ub_chat\\] fail" "$pair_label responder fail" || return 1
  assert_log_has "$responder_log" "\\[ub_chat\\] summary tx=5 rx=5" "$pair_label responder summary" || return 1
  assert_log_has "$responder_log" "\\[CHAT\\] responder seq=[0-9]+ \"greeting from initiator\"" "$pair_label responder payload" || return 1
}

cleanup_tmux_session() {
  local cleanup_script="$2"

  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

send_chat_cmd() {
  local role="$1"
  local local_ip="$2"
  local peer_ip="$3"
  local serial_port="$4"
  local start_marker="$5"
  local payload

  payload=$'export LINQU_UB_ROLE='"${role}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_PEER_IP='"${peer_ip}"$'\n'
  payload+=$'export LINQU_UB_TIMEOUT_S=120\n'
  payload+=$'export LINQU_UB_POST_SYNC_SETTLE_MS=0\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_chat\n'

  send_serial_block "$serial_port" "$payload" || return 1
}

wait_for_chat_bind_ready() {
  local file="$1"
  local timeout_s="$2"
  wait_for_log_pattern "$file" "\[ub_chat\] chat socket bind ok port=18556" "$timeout_s"
}

capture_guest_diag() {
  local node_id="$1"
  local serial_port="$2"

  send_serial_line "$serial_port" "echo DIAG_${node_id}_START" || true
  sleep 0.2
  send_serial_line "$serial_port" "cat /sys/class/net/ipourma0/query_ipourma_stats" || true
  sleep 0.2
  send_serial_line "$serial_port" "ifconfig ipourma0" || true
  sleep 0.2
  send_serial_line "$serial_port" "echo DIAG_${node_id}_END" || true
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

prepare_single_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  ENV_FILE="$ENV_FILE" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" \
    UB_SIM_PORT_NUM=7 "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null
  source "$ENV_FILE"
  RUN_DIR="$RUN_DIR"
  CLEANUP_SCRIPT="$CLEANUP_SCRIPT"
  PORT_BASE="$PORT_BASE"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "prepare: wait shell gate $node_id"
    if ! wait_for_guest_shell "$guest_log" "$BOOT_WAIT_SECS"; then
      trace "prepare: shell gate timeout $node_id"
      echo "$node_id did not finish shell boot gate" >&2
      cleanup_tmux_session "" "$CLEANUP_SCRIPT"
      return 1
    fi
    trace "prepare: shell gate ok $node_id"
  done

  sleep 2
  trace "prepare: environment ready"

  return 0
}

run_pair_iteration() {
  local initiator="$2"
  local responder="$3"
  local initiator_log="$RUN_DIR/${initiator}_guest.log"
  local responder_log="$RUN_DIR/${responder}_guest.log"
  local initiator_slice="$OUT_DIR/eight_node_chat_${initiator}_${responder}_initiator.slice.log"
  local responder_slice="$OUT_DIR/eight_node_chat_${initiator}_${responder}_responder.slice.log"
  local rc=0
  local initiator_ip responder_ip
  local initiator_serial_port responder_serial_port
  local initiator_start_line=0
  local responder_start_line=0
  local initiator_start_marker responder_start_marker

  initiator_ip="$(node_ip "$initiator")"
  responder_ip="$(node_ip "$responder")"
  initiator_serial_port="$(node_serial_port "$initiator" "$PORT_BASE")"
  responder_serial_port="$(node_serial_port "$responder" "$PORT_BASE")"
  initiator_start_marker="CHAT_CMD_INITIATOR_START_${pair_idx}"
  responder_start_marker="CHAT_CMD_RESPONDER_START_${pair_idx}"
  trace "pair ${initiator}-${responder}: start"

  [[ -f "$initiator_log" ]] && initiator_start_line="$(wc -l < "$initiator_log")"
  [[ -f "$responder_log" ]] && responder_start_line="$(wc -l < "$responder_log")"

  trace "pair ${initiator}-${responder}: send responder cmd"
  if ! send_chat_cmd responder "$responder_ip" "$initiator_ip" "$responder_serial_port" "$responder_start_marker"; then
    trace "pair ${initiator}-${responder}: responder cmd send failed"
    echo "$initiator-$responder: failed to send responder chat command" >&2
    return 1
  fi
  trace "pair ${initiator}-${responder}: wait responder marker"
  if ! wait_for_log_pattern "$responder_log" "$responder_start_marker" 30; then
    trace "pair ${initiator}-${responder}: responder marker timeout"
    echo "$initiator-$responder: responder start marker did not appear" >&2
    return 1
  fi
  trace "pair ${initiator}-${responder}: wait responder bind"
  if ! wait_for_chat_bind_ready "$responder_log" 30; then
    trace "pair ${initiator}-${responder}: responder bind timeout"
    echo "$initiator-$responder: responder chat socket did not bind in time" >&2
    return 1
  fi
  sleep "$START_GAP_SECS"
  trace "pair ${initiator}-${responder}: send initiator cmd"
  if ! send_chat_cmd initiator "$initiator_ip" "$responder_ip" "$initiator_serial_port" "$initiator_start_marker"; then
    trace "pair ${initiator}-${responder}: initiator cmd send failed"
    echo "$initiator-$responder: failed to send initiator chat command" >&2
    return 1
  fi
  trace "pair ${initiator}-${responder}: wait initiator marker"
  if ! wait_for_log_pattern "$initiator_log" "$initiator_start_marker" 30; then
    trace "pair ${initiator}-${responder}: initiator marker timeout"
    echo "$initiator-$responder: initiator start marker did not appear" >&2
    return 1
  fi

  rc=0
  trace "pair ${initiator}-${responder}: wait initiator pass"
  wait_for_log_pass_or_fail_since "$initiator_log" "$initiator_start_line" "\\[ub_chat\\] pass" "\\[ub_chat\\] fail" "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    trace "pair ${initiator}-${responder}: initiator pass wait failed rc=$rc"
    capture_guest_diag "$initiator" "$initiator_serial_port"
    capture_guest_diag "$responder" "$responder_serial_port"
    sleep 2
    echo "$initiator-$responder: initiator chat did not pass" >&2
    return 1
  fi

  rc=0
  trace "pair ${initiator}-${responder}: wait responder pass"
  wait_for_log_pass_or_fail_since "$responder_log" "$responder_start_line" "\\[ub_chat\\] pass" "\\[ub_chat\\] fail" "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    trace "pair ${initiator}-${responder}: responder pass wait failed rc=$rc"
    capture_guest_diag "$initiator" "$initiator_serial_port"
    capture_guest_diag "$responder" "$responder_serial_port"
    sleep 2
    echo "$initiator-$responder: responder chat did not pass" >&2
    return 1
  fi

  slice_log_since "$initiator_log" "$initiator_start_line" "$initiator_slice"
  slice_log_since "$responder_log" "$responder_start_line" "$responder_slice"

  validate_chat_logs "$initiator" "$responder" "$initiator_slice" "$responder_slice" || {
    trace "pair ${initiator}-${responder}: validate logs failed"
    return 1
  }

  trace "pair ${initiator}-${responder}: pass"

  return 0
}

mkdir -p "$OUT_DIR"
{
  echo "eight-node chat matrix"
  echo "run_id_base=$RUN_ID_BASE"
  echo "run_dir=$RUN_DIR"
  echo "boot_wait_secs=$BOOT_WAIT_SECS"
  echo "pair_wait_secs=$PAIR_WAIT_SECS"
  echo "start_gap_secs=$START_GAP_SECS"
  echo
} > "$REPORT_FILE"

if ! prepare_single_environment; then
  exit 1
fi

pair_idx=1
for pair in "${PAIR_LIST[@]}"; do
  initiator="${pair%% *}"
  responder="${pair##* }"
  if run_pair_iteration "$pair_idx" "$initiator" "$responder"; then
    echo "$initiator-$responder: PASS" | tee -a "$REPORT_FILE"
  else
    echo "$initiator-$responder: FAIL" | tee -a "$REPORT_FILE"
    exit 1
  fi
  pair_idx=$((pair_idx + 1))
  echo >> "$REPORT_FILE"
done

  cleanup_tmux_session "" "$CLEANUP_SCRIPT"
  echo "result=PASS" | tee -a "$REPORT_FILE"
