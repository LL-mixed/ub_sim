#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_obmm_import_stress.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_obmm_import_stress.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmm_import_stress8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
APP_WAIT_SECS="${APP_WAIT_SECS:-300}"
START_GAP_SECS="${START_GAP_SECS:-0}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
PORT_BASE_START="${PORT_BASE_START:-$((53600 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
STRESS_SIZE="${STRESS_SIZE:-1048576}"
STRESS_PATTERN="${STRESS_PATTERN:-mixed}"
STRESS_ITERS="${STRESS_ITERS:-256}"
STRESS_FLUSH="${STRESS_FLUSH:-none}"
STRESS_CHUNK_SIZE="${STRESS_CHUNK_SIZE:-64}"
STRESS_VERIFY="${STRESS_VERIFY:-1}"
STRESS_GVA_MODE="${STRESS_GVA_MODE:-legacy}"
STRESS_GVA_CACHE_POLICY="${STRESS_GVA_CACHE_POLICY:-wt}"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)

trace() {
  local msg="$1"
  printf '[obmm-import-stress8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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
  wait_for_log_pattern "$file" "\\[run_app\\] entering interactive shell" "$timeout_s"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

stress_args() {
  local peer_idx="$1"
  local args

  args="--size $STRESS_SIZE --pattern $STRESS_PATTERN --iterations $STRESS_ITERS"
  args+=" --flush-mode $STRESS_FLUSH --chunk-size $STRESS_CHUNK_SIZE"
  args+=" --node-count 8 --peer-index $peer_idx"
  args+=" --gva-mode $STRESS_GVA_MODE --gva-cache-policy $STRESS_GVA_CACHE_POLICY"
  if [[ "$STRESS_VERIFY" == "1" ]]; then
    args+=" --verify"
  fi
  echo "$args"
}

send_obmm_import_stress_cmd() {
  local local_ip="$1"
  local peer_idx="$2"
  local serial_endpoint="$3"
  local start_marker="$4"
  local payload

  payload=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_obmm_import_stress '"$(stress_args "$peer_idx")"$'\n'

  send_serial_block "$serial_endpoint" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP size=$STRESS_SIZE iters=$STRESS_ITERS gva_mode=$STRESS_GVA_MODE"
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
  local local_idx="$3"
  local peer_idx="$4"

  assert_log_has "$log_file" "\\[obmm_import_stress\\] local_idx=${local_idx} peer_idx=${peer_idx} node_count=8" \
    "$node_id node identity" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] bootstrap lookup ok got_count=8 node_count=8 peer_got=1" \
    "$node_id bootstrap lookup" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] import ok mem_id=" \
    "$node_id import" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] setup complete import_va=" \
    "$node_id setup complete" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] completion barrier ok generation=" \
    "$node_id completion barrier" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] result=done " \
    "$node_id result" || return 1
  assert_log_has "$log_file" "\\[obmm_import_stress\\] gva_mode=" \
    "$node_id gva mode" || return 1
  assert_log_absent "$log_file" "\\[obmm_import_stress\\] .*failed" "$node_id failed marker" || return 1
  assert_log_absent "$log_file" "WARNING: CPU:" "$node_id kernel warning" || return 1
  assert_log_absent "$log_file" "Call trace:" "$node_id call trace" || return 1
  assert_log_absent "$log_file" "Kernel panic - not syncing" "$node_id kernel panic" || return 1
}

run_import_stress_app() {
  local node_id
  local guest_log
  local serial_endpoint
  local start_marker
  local start_line
  local local_idx
  local peer_idx
  local rc
  typeset -A START_LINES
  typeset -A PEER_INDEXES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    local_idx=$(( $(node_index "$node_id") - 1 ))
    peer_idx=$(( (local_idx + 1) % 8 ))
    PEER_INDEXES[$node_id]="$peer_idx"
    start_line=$(wc -l < "$guest_log" | tr -d ' ')
    START_LINES[$node_id]="$start_line"
    serial_endpoint="$(node_serial_endpoint "$node_id" "$PORT_BASE")"
    start_marker="OBMM_IMPORT_STRESS_${node_id}_START"
    trace "start import stress on $node_id peer_idx=$peer_idx serial=$serial_endpoint"
    send_obmm_import_stress_cmd "$(node_ip "$node_id")" "$peer_idx" "$serial_endpoint" "$start_marker"
    sleep "$START_GAP_SECS"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    rc=0
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[obmm_import_stress\\] result=done" \
      "\\[obmm_import_stress\\] .*failed|unknown option|Kernel panic - not syncing" \
      "$APP_WAIT_SECS" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      trace "FAIL: import stress app did not pass on $node_id rc=$rc"
      return 1
    fi
  done

  for node_id in "${NODE_IDS[@]}"; do
    local_idx=$(( $(node_index "$node_id") - 1 ))
    validate_node_log "$node_id" "$RUN_DIR/${node_id}_guest.log" \
      "$local_idx" "${PEER_INDEXES[$node_id]}" || return 1
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
    if run_import_stress_app; then
      result="PASS"
    fi
  fi

  {
    echo "run_id=$RUN_ID_BASE"
    echo "result=$result"
    echo "run_dir=$RUN_DIR"
    echo "stress_size=$STRESS_SIZE"
    echo "stress_iters=$STRESS_ITERS"
    echo "stress_gva_mode=$STRESS_GVA_MODE"
    [[ -n "$cleanup_script" ]] && echo "cleanup_script=$cleanup_script"
  } | tee "$REPORT_FILE"

  if [[ -n "$cleanup_script" ]]; then
    cleanup_headless_env "$cleanup_script"
  fi

  [[ "$result" == "PASS" ]]
}

main "$@"
