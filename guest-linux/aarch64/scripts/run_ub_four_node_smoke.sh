#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
ITERATIONS="${ITERATIONS:-1}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-120}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_smoke4_${RANDOM}}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/four_node_smoke_report.latest.txt}"

NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)

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

validate_guest_log() {
  local node_id="$1"
  local local_ip="$2"
  local guest_log="$3"

  assert_log_has "$guest_log" "\\[init\\] ipourma bootstrap iface=ipourma0 ifindex=[0-9]+ local=${local_ip} peer=\\(none\\)" \
    "${node_id} ipourma bootstrap" || return 1
  assert_log_has "$guest_log" "\\[init\\] switching to /bin/run_demo boot flow" \
    "${node_id} enter run_demo" || return 1
  assert_log_has "$guest_log" "\\[run_demo\\] boot flow completed, dropping to shell" \
    "${node_id} shell reached" || return 1
  assert_log_absent "$guest_log" "Kernel panic - not syncing" \
    "${node_id} kernel panic" || return 1
  assert_log_absent "$guest_log" "WARNING: CPU:" \
    "${node_id} kernel warning" || return 1
  assert_log_absent "$guest_log" "Call trace:" \
    "${node_id} stacktrace" || return 1
}

validate_qemu_log() {
  local node_id="$1"
  local qemu_log="$2"

  assert_log_has "$qemu_log" "ub route table done ubcdev0 entry_num=7" \
    "${node_id} route table entry count" || return 1
  assert_log_has "$qemu_log" "ub_fm: reconciled local fabric config for ubcdev0 cna=0x[0-9a-f]+ ports=3" \
    "${node_id} fm reconciled" || return 1
  assert_log_has "$qemu_log" "entity_plan: apply completed" \
    "${node_id} entity plan applied" || return 1
}

cleanup_session() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

run_iteration() {
  local iter="$1"
  local run_id="${RUN_ID_BASE}_iter${iter}"
  local session_name="ub-four-node-${run_id}"
  local run_dir="$LOG_DIR/${run_id}_tmux4"
  local cleanup_script="$OUT_DIR/tmux_four_node_cleanup.${run_id}.sh"
  local result=0

  TMUX_ATTACH=0 RUN_ID="$run_id" TMUX_SESSION_NAME="$session_name" \
    "$SCRIPT_DIR/launch_ub_four_node_tmux.sh"

  for idx in {1..4}; do
    local guest_log="$run_dir/${NODE_IDS[$idx]}_guest.log"
    if ! wait_for_log_pattern "$guest_log" "\\[run_demo\\] boot flow completed, dropping to shell" "$BOOT_WAIT_SECS"; then
      echo "iteration ${iter}: ${NODE_IDS[$idx]} did not reach shell within ${BOOT_WAIT_SECS}s" >&2
      result=1
      break
    fi
  done

  if [[ "$result" -eq 0 ]]; then
    for idx in {1..4}; do
      local node_id="${NODE_IDS[$idx]}"
      local local_ip="${NODE_IPS[$idx]}"
      local guest_log="$run_dir/${node_id}_guest.log"
      local qemu_log="$run_dir/${node_id}_qemu.log"
      validate_guest_log "$node_id" "$local_ip" "$guest_log" || result=1
      validate_qemu_log "$node_id" "$qemu_log" || result=1
      [[ "$result" -eq 0 ]] || break
    done
  fi

  cleanup_session "$cleanup_script"
  return "$result"
}

pass_count=0
fail_count=0
{
  echo "four-node smoke"
  echo "run_id_base=$RUN_ID_BASE"
  echo "iterations=$ITERATIONS"
  echo "boot_wait_secs=$BOOT_WAIT_SECS"
  echo
} > "$REPORT_FILE"

for iter in $(seq 1 "$ITERATIONS"); do
  if run_iteration "$iter"; then
    echo "iteration ${iter}: PASS"
    echo "iteration ${iter}: PASS" >> "$REPORT_FILE"
    pass_count=$((pass_count + 1))
  else
    echo "iteration ${iter}: FAIL" >&2
    echo "iteration ${iter}: FAIL" >> "$REPORT_FILE"
    fail_count=$((fail_count + 1))
  fi
done

{
  echo
  echo "passed=$pass_count"
  echo "failed=$fail_count"
} >> "$REPORT_FILE"

echo "Passed: ${pass_count} / ${ITERATIONS}" | tee -a "$REPORT_FILE"
[[ "$fail_count" -eq 0 ]]
