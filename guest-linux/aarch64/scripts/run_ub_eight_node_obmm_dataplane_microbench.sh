#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_obmm_dataplane_microbench.latest.txt}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_obmm_dataplane_microbench.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_dp_microbench8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
MODE_WAIT_SECS="${MODE_WAIT_SECS:-240}"
START_GAP_SECS="${START_GAP_SECS:-0}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=25% obmm.mempool_size=0}"
QEMU_MEM="${QEMU_MEM:-8G}"
QEMU_SMP="${QEMU_SMP:-4}"
PORT_BASE_START="${PORT_BASE_START:-$((53600 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
DP_MODES=(${=DP_MODES_OVERRIDE:-legacy-pa generic-gva gsva})
DP_SIZE="${DP_SIZE:-2097152}"
DP_ITERS="${DP_ITERS:-8192}"
DP_CHUNK_SIZE="${DP_CHUNK_SIZE:-64}"
DP_VERIFY="${DP_VERIFY:-0}"
DP_GENERIC_PTE_OFFSET="${DP_GENERIC_PTE_OFFSET:-0x1000}"
DP_GSVA_BASE="${DP_GSVA_BASE:-0x700000000000}"
DP_GSVA_GENERATION_BASE="${DP_GSVA_GENERATION_BASE:-0x44504d424701}"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)

trace() {
  local msg="$1"
  printf '[dp-microbench8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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

mode_generation() {
  local mode="$1"
  case "$mode" in
    legacy-pa|legacy) echo "$DP_GSVA_GENERATION_BASE" ;;
    generic-gva|generic|gva) printf '0x%x\n' "$((DP_GSVA_GENERATION_BASE + 1))" ;;
    gsva) printf '0x%x\n' "$((DP_GSVA_GENERATION_BASE + 2))" ;;
    *) echo "$DP_GSVA_GENERATION_BASE" ;;
  esac
}

bench_args() {
  local mode="$1"
  local peer_idx="$2"
  local args

  args="--mode $mode --size $DP_SIZE --iterations $DP_ITERS --chunk-size $DP_CHUNK_SIZE"
  args+=" --node-count 8 --peer-index $peer_idx"
  args+=" --generic-pte-offset $DP_GENERIC_PTE_OFFSET"
  args+=" --gsva-base $DP_GSVA_BASE --gsva-generation $(mode_generation "$mode")"
  if [[ "$DP_VERIFY" == "1" ]]; then
    args+=" --verify"
  fi
  echo "$args"
}

send_microbench_cmd() {
  local local_ip="$1"
  local mode="$2"
  local peer_idx="$3"
  local serial_endpoint="$4"
  local start_marker="$5"
  local payload

  payload=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_ub_obmm_dataplane_microbench '"$(bench_args "$mode" "$peer_idx")"$'\n'

  send_serial_block "$serial_endpoint" "$payload"
}

prepare_environment() {
  local guest_log
  local node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE qemu_mem=$QEMU_MEM qemu_smp=$QEMU_SMP modes=${(j:,:)DP_MODES}"
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

validate_node_mode_log() {
  local node_id="$1"
  local log_file="$2"
  local start_line="$3"
  local mode="$4"
  local local_idx="$5"
  local peer_idx="$6"

  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] local_idx=${local_idx} peer_idx=${peer_idx} node_count=8" \
    "$node_id $mode node identity" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] bootstrap lookup ok got_count=8 node_count=8 peer_got=1" \
    "$node_id $mode bootstrap" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] import ok mem_id=" \
    "$node_id $mode import" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] setup complete import_va=" \
    "$node_id $mode setup" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] completion barrier ok generation=.*got_count=8 node_count=8" \
    "$node_id $mode completion" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] result=done mode=${mode} .*verify_failures=0" \
    "$node_id $mode result" || return 1
  assert_log_has_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] mapping mode=${mode} import_mem_id=" \
    "$node_id $mode mapping" || return 1
  assert_log_absent_since "$log_file" "$start_line" "\\[obmm_dataplane_microbench\\] .*failed|verify_failures=[1-9]" \
    "$node_id $mode failure" || return 1
  assert_log_absent_since "$log_file" "$start_line" "Kernel panic - not syncing" \
    "$node_id $mode kernel panic" || return 1
}

run_mode() {
  local mode="$1"
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

  trace "start mode=$mode"
  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    local_idx=$(( $(node_index "$node_id") - 1 ))
    peer_idx=$(( (local_idx + 1) % 8 ))
    PEER_INDEXES[$node_id]="$peer_idx"
    start_line=$(wc -l < "$guest_log" | tr -d ' ')
    START_LINES[$node_id]="$start_line"
    serial_endpoint="$(node_serial_endpoint "$node_id" "$PORT_BASE")"
    start_marker="OBMM_DP_MICROBENCH_${mode}_${node_id}_START"
    trace "start $mode on $node_id peer_idx=$peer_idx serial=$serial_endpoint"
    send_microbench_cmd "$(node_ip "$node_id")" "$mode" "$peer_idx" "$serial_endpoint" "$start_marker"
    sleep "$START_GAP_SECS"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    rc=0
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[obmm_dataplane_microbench\\] result=done mode=${mode}" \
      "\\[obmm_dataplane_microbench\\] .*failed|verify_failures=[1-9]|Kernel panic - not syncing" \
      "$MODE_WAIT_SECS" || rc=$?
    if [[ "$rc" -ne 0 ]]; then
      trace "FAIL: $mode did not pass on $node_id rc=$rc"
      return 1
    fi
  done

  for node_id in "${NODE_IDS[@]}"; do
    local_idx=$(( $(node_index "$node_id") - 1 ))
    validate_node_mode_log "$node_id" "$RUN_DIR/${node_id}_guest.log" \
      "${START_LINES[$node_id]}" "$mode" "$local_idx" "${PEER_INDEXES[$node_id]}" || return 1
  done
  trace "mode=$mode passed on all eight nodes"
  return 0
}

run_all_modes() {
  local mode

  for mode in "${DP_MODES[@]}"; do
    run_mode "$mode" || return 1
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
    if run_all_modes; then
      result="PASS"
    fi
  fi

  {
    echo "run_id=$RUN_ID_BASE"
    echo "result=$result"
    echo "run_dir=$RUN_DIR"
    echo "modes=${(j:,:)DP_MODES}"
    echo "dp_size=$DP_SIZE"
    echo "dp_iters=$DP_ITERS"
    [[ -n "$cleanup_script" ]] && echo "cleanup_script=$cleanup_script"
  } | tee "$REPORT_FILE"

  if [[ -n "$cleanup_script" ]]; then
    cleanup_headless_env "$cleanup_script"
  fi

  [[ "$result" == "PASS" ]]
}

main "$@"
