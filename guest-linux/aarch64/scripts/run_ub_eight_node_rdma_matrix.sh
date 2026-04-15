#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_rdma_matrix.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_rdma_matrix.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_rdma8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
CLEANUP_SCRIPT="$OUT_DIR/headless_eight_node_cleanup.${RUN_ID_BASE}.sh"
ENV_FILE="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
PAIR_WAIT_SECS="${PAIR_WAIT_SECS:-180}"
START_GAP_SECS="${START_GAP_SECS:-1}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((59850 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
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

trace() {
  local msg="$1"
  printf '[rdma8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

send_rdma_cmd() {
  local role="$1"
  local local_ip="$2"
  local peer_ip="$3"
  local serial_port="$4"
  local start_marker="$5"
  local payload

  payload=$'export LINQU_UB_ROLE='"${role}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_PEER_IP='"${peer_ip}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_rdma_demo\n'

  send_serial_block "$serial_port" "$payload"
}

capture_guest_diag() {
  local node_id="$1"
  local serial_port="$2"
  send_serial_block "$serial_port" $'echo DIAG_'"${node_id}"$'_START\nifconfig ipourma0\nps\n'
}

validate_rdma_slice() {
  local src="$1"
  local dst="$2"
  local role="$3"
  local log_file="$4"

  assert_log_has "$log_file" "\\[ub_rdma\\] pass" "$src->$dst $role pass" || return 1
  assert_log_absent "$log_file" "\\[ub_rdma\\] fail" "$src->$dst $role fail" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 2: alloc_ummu_tid -> ok" "$src->$dst $role alloc tid" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 2: alloc_token_id -> ok" "$src->$dst $role alloc token" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 7: register_seg -> ok" "$src->$dst $role register seg" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 8: import_jetty -> ok" "$src->$dst $role import jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9: bind_jetty -> ok" "$src->$dst $role bind jetty" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: post_recv -> ok" "$src->$dst $role post recv" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: ready_sync -> ok" "$src->$dst $role ready sync" || return 1
  if [[ "$role" == "initiator" ]]; then
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: send_request -> ok len=[0-9]+" "$src->$dst initiator send request" || return 1
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: recv_reply -> ok payload=\"rdma reply payload from responder\"" "$src->$dst initiator recv reply" || return 1
  else
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: recv_request -> ok payload=\"rdma request payload from initiator\"" "$src->$dst responder recv request" || return 1
    assert_log_has "$log_file" "\\[ub_rdma\\] step 9\\.5: send_reply -> ok len=[0-9]+" "$src->$dst responder send reply" || return 1
  fi
  assert_log_has "$log_file" "\\[ub_rdma\\] step 10: unbind_jetty -> ok" "$src->$dst $role unbind" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] step 10: unimport_jetty -> ok" "$src->$dst $role unimport" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: unregister_seg -> ok" "$src->$dst $role unregister" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: free_token_id -> ok" "$src->$dst $role free token" || return 1
  assert_log_has "$log_file" "\\[ub_rdma\\] cleanup: free_ummu_tid -> ok" "$src->$dst $role free tid" || return 1
  assert_log_absent "$log_file" "wait resp timeout|failed to query device status|invalidate cfg_table failed|ubcore_unimport_jetty_async failed|failed to remove uobject|WARNING: CPU:|Call trace:|Kernel panic - not syncing" "$src->$dst $role kernel health" || return 1
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

run_directed_call() {
  local call_idx="$1"
  local src="$2"
  local dst="$3"
  local src_ip dst_ip src_serial dst_serial src_log dst_log
  local src_start_line=0 dst_start_line=0
  local initiator_marker responder_marker
  local initiator_slice responder_slice rc=0

  src_ip="$(node_ip "$src")"
  dst_ip="$(node_ip "$dst")"
  src_serial="$(node_serial_port "$src" "$PORT_BASE")"
  dst_serial="$(node_serial_port "$dst" "$PORT_BASE")"
  src_log="$RUN_DIR/${src}_guest.log"
  dst_log="$RUN_DIR/${dst}_guest.log"
  initiator_marker="RDMA_INITIATOR_START_${call_idx}_${src}_${dst}"
  responder_marker="RDMA_RESPONDER_START_${call_idx}_${src}_${dst}"
  initiator_slice="$OUT_DIR/eight_node_rdma_${src}_${dst}_initiator.slice.log"
  responder_slice="$OUT_DIR/eight_node_rdma_${src}_${dst}_responder.slice.log"

  [[ -f "$src_log" ]] && src_start_line="$(wc -l < "$src_log")"
  [[ -f "$dst_log" ]] && dst_start_line="$(wc -l < "$dst_log")"

  trace "call ${src}->${dst}: launch responder"
  if ! send_rdma_cmd responder "$dst_ip" "$src_ip" "$dst_serial" "$responder_marker"; then
    echo "$src->$dst: failed to send responder command" >&2
    return 1
  fi
  if ! wait_for_log_pattern "$dst_log" "$responder_marker" 20; then
    echo "$src->$dst: missing responder start marker" >&2
    return 1
  fi
  sleep "$START_GAP_SECS"

  trace "call ${src}->${dst}: launch initiator"
  if ! send_rdma_cmd initiator "$src_ip" "$dst_ip" "$src_serial" "$initiator_marker"; then
    echo "$src->$dst: failed to send initiator command" >&2
    return 1
  fi
  if ! wait_for_log_pattern "$src_log" "$initiator_marker" 20; then
    echo "$src->$dst: missing initiator start marker" >&2
    return 1
  fi

  rc=0
  wait_for_log_pass_or_fail_since "$src_log" "$src_start_line" "\\[ub_rdma\\] pass" "\\[ub_rdma\\] fail" "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    capture_guest_diag "$src" "$src_serial"
    capture_guest_diag "$dst" "$dst_serial"
    echo "$src->$dst: initiator did not pass" >&2
    return 1
  fi

  rc=0
  wait_for_log_pass_or_fail_since "$dst_log" "$dst_start_line" "\\[ub_rdma\\] pass" "\\[ub_rdma\\] fail" "$PAIR_WAIT_SECS" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    capture_guest_diag "$src" "$src_serial"
    capture_guest_diag "$dst" "$dst_serial"
    echo "$src->$dst: responder did not pass" >&2
    return 1
  fi

  slice_log_since "$src_log" "$src_start_line" "$initiator_slice"
  slice_log_since "$dst_log" "$dst_start_line" "$responder_slice"

  validate_rdma_slice "$src" "$dst" initiator "$initiator_slice" || return 1
  validate_rdma_slice "$src" "$dst" responder "$responder_slice" || return 1
  trace "call ${src}->${dst}: pass"
}

chmod +x "$0" 2>/dev/null || true
mkdir -p "$OUT_DIR"
{
  echo "eight-node rdma matrix"
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

cleanup_headless_env "$CLEANUP_SCRIPT"
echo "result=PASS" | tee -a "$REPORT_FILE"
