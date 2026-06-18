#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_gva_direct.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_gva_direct.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gva_direct8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
APP_WAIT_SECS="${APP_WAIT_SECS:-120}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=25%}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
PORT_BASE_START="${PORT_BASE_START:-$((54000 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
GVA_DIRECT_MODE="${GVA_DIRECT_MODE:-write-read}"
GVA_DIRECT_LOCAL_VA="${GVA_DIRECT_LOCAL_VA:-0x710000000000}"
GVA_DIRECT_HOME_VA="${GVA_DIRECT_HOME_VA:-0x720000000000}"
GVA_DIRECT_SIZE="${GVA_DIRECT_SIZE:-0x400000}"
GVA_DIRECT_NODE_COUNT=8

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)

trace() {
  local msg="$1"
  printf '[gva-direct8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! rg -q "$pattern" "$file"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

node_index() {
  case "$1" in
    nodeA) echo 0 ;;
    nodeB) echo 1 ;;
    nodeC) echo 2 ;;
    nodeD) echo 3 ;;
    nodeE) echo 4 ;;
    nodeF) echo 5 ;;
    nodeG) echo 6 ;;
    nodeH) echo 7 ;;
    *) return 1 ;;
  esac
}

node_serial_port() {
  local node_id="$1"
  local port_base="$2"
  local idx="$(node_index "$node_id")"
  echo $((port_base + 32 + idx))
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

send_gva_direct_cmd() {
  local serial_endpoint="$1"
  local start_marker="$2"
  local node_idx="$3"
  local payload

  payload=$'echo '"${start_marker}"$'\n'
  payload+="/bin/linqu_gva_direct --mode ${GVA_DIRECT_MODE} --node-count ${GVA_DIRECT_NODE_COUNT} --node-idx ${node_idx} --size ${GVA_DIRECT_SIZE} --local-va ${GVA_DIRECT_LOCAL_VA} --home-va ${GVA_DIRECT_HOME_VA}"$'\n'

  send_serial_block "$serial_endpoint" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP"
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
  local guest_log="$2"
  local qemu_log="$3"
  local start_line="$4"
  local node_idx="$5"

  assert_log_has_since "$guest_log" "$start_line" "\\[gva_direct\\] start mode=${GVA_DIRECT_MODE} node=${node_idx} .*node_count=${GVA_DIRECT_NODE_COUNT}" \
    "$node_id start" || return 1
  if [[ "$node_idx" -eq 0 ]]; then
    assert_log_has_since "$guest_log" "$start_line" "\\[gva_direct\\] result=done mode=${GVA_DIRECT_MODE} role=home node_count=${GVA_DIRECT_NODE_COUNT} peers=7" \
      "$node_id home result" || return 1
  else
    assert_log_has_since "$guest_log" "$start_line" "\\[gva_direct\\] result=done mode=${GVA_DIRECT_MODE} role=peer node=${node_idx} node_count=${GVA_DIRECT_NODE_COUNT}" \
      "$node_id peer result" || return 1
    assert_log_has "$qemu_log" "GVA_S3_MAP" "$node_id GVA route" || return 1
    assert_log_has "$qemu_log" "GVA_PATH" "$node_id GVA datapath" || return 1
  fi
  assert_log_absent_since "$guest_log" "$start_line" "\\[gva_direct\\] result=fail|linqu_gva_direct failed|Kernel panic - not syncing|Call trace:" \
    "$node_id failure" || return 1
}

run_gva_direct_app() {
  local node_id
  local guest_log
  local serial_endpoint
  local start_marker
  local start_line
  local node_idx
  local rc
  typeset -A START_LINES

  if [[ "$GVA_DIRECT_MODE" != "write-read" ]]; then
    echo "[gva-direct8] FAIL: eight-node validation supports write-read only" >&2
    return 1
  fi

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_line=$(wc -l < "$guest_log" | tr -d ' ')
    START_LINES[$node_id]="$start_line"
    node_idx="$(node_index "$node_id")"
    serial_endpoint="$(node_serial_endpoint "$node_id" "$PORT_BASE")"
    start_marker="GVA_DIRECT_${node_id}_START"
    trace "start GVA direct on $node_id idx=$node_idx serial=$serial_endpoint"
    send_gva_direct_cmd "$serial_endpoint" "$start_marker" "$node_idx"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    rc=0
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[gva_direct\\] result=done" "\\[gva_direct\\] result=fail|linqu_gva_direct failed|Kernel panic - not syncing|Call trace:" \
      "$APP_WAIT_SECS" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      trace "FAIL: GVA direct did not pass on $node_id rc=$rc"
      return 1
    fi
  done

  for node_id in "${NODE_IDS[@]}"; do
    node_idx="$(node_index "$node_id")"
    validate_node_log "$node_id" "$RUN_DIR/${node_id}_guest.log" \
      "$RUN_DIR/${node_id}_qemu.log" "${START_LINES[$node_id]}" "$node_idx" || return 1
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
    if run_gva_direct_app; then
      result="PASS"
    fi
  fi

  {
    echo "run_id=$RUN_ID_BASE"
    echo "result=$result"
    echo "run_dir=$RUN_DIR"
    echo "mode=$GVA_DIRECT_MODE"
    echo "node_count=$GVA_DIRECT_NODE_COUNT"
    [[ -n "$cleanup_script" ]] && echo "cleanup_script=$cleanup_script"
  } | tee "$REPORT_FILE"

  if [[ -n "$cleanup_script" ]]; then
    cleanup_headless_env "$cleanup_script"
  fi

  [[ "$result" == "PASS" ]]
}

main "$@"
