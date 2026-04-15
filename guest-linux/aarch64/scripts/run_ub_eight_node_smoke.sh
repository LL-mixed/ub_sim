#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_smoke8_${RANDOM}}"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/eight_node_smoke_report.latest.txt}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
PORT_NUM="${UB_SIM_PORT_NUM:-7}"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)

extract_eid_suffix() {
  local qemu_log="$1"
  rg -o "ubc ue2ue seid resp: 1 eid fe80::[0-9]+" "$qemu_log" | tail -n1 | sed -E 's/.*fe80::([0-9]+)/\1/'
}

extract_cna_hex() {
  local qemu_log="$1"
  rg -o "ub_fm: reconciled local fabric config for ubcdev0 cna=0x[0-9a-f]+" "$qemu_log" | tail -n1 | sed -E 's/.*cna=(0x[0-9a-f]+)/\1/'
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
  assert_log_has "$guest_log" "\\[run_demo\\] boot flow completed, dropping to shell" \
    "${node_id} shell reached" || return 1
  assert_log_absent "$guest_log" "Kernel panic - not syncing" "${node_id} kernel panic" || return 1
  assert_log_absent "$guest_log" "WARNING: CPU:" "${node_id} kernel warning" || return 1
  assert_log_absent "$guest_log" "Call trace:" "${node_id} stacktrace" || return 1
}

validate_qemu_log() {
  local node_id="$1"
  local qemu_log="$2"

  assert_log_has "$qemu_log" "virt ubc: port_num=7" "${node_id} configured port_num" || return 1
  assert_log_has "$qemu_log" "ub_fm: reconciled local fabric config for ubcdev0 cna=0x[0-9a-f]+ ports=7" \
    "${node_id} fm reconciled ports=7" || return 1
  assert_log_has "$qemu_log" "entity_plan: apply completed" "${node_id} entity plan applied" || return 1
}

validate_unique_fabric_identity() {
  local run_dir="$1"
  local idx node_id qemu_log eid_suffix cna
  local -A seen_eids=()
  local -A seen_cnas=()

  for idx in {1..8}; do
    node_id="${NODE_IDS[$idx]}"
    qemu_log="$run_dir/${node_id}_qemu.log"
    eid_suffix="$(extract_eid_suffix "$qemu_log")"
    cna="$(extract_cna_hex "$qemu_log")"

    if [[ -z "$eid_suffix" ]]; then
      echo "missing eid suffix in $qemu_log" >&2
      return 1
    fi
    if [[ -z "$cna" ]]; then
      echo "missing cna in $qemu_log" >&2
      return 1
    fi
    if [[ -n "${seen_eids[$eid_suffix]-}" ]]; then
      echo "duplicate eid suffix fe80::${eid_suffix} in ${node_id} and ${seen_eids[$eid_suffix]}" >&2
      return 1
    fi
    if [[ -n "${seen_cnas[$cna]-}" ]]; then
      echo "duplicate cna ${cna} in ${node_id} and ${seen_cnas[$cna]}" >&2
      return 1
    fi

    seen_eids[$eid_suffix]="$node_id"
    seen_cnas[$cna]="$node_id"
  done
}

cleanup_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

main() {
  local run_dir="$LOG_DIR/${RUN_ID_BASE}_headless8"
  local env_file="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
  local cleanup_script=""
  local result=0
  local idx node_id guest_log qemu_log

  mkdir -p "$OUT_DIR"
  {
    echo "eight-node smoke"
    echo "run_id=$RUN_ID_BASE"
    echo "boot_wait_secs=$BOOT_WAIT_SECS"
    echo "ub_sim_port_num=$PORT_NUM"
    echo
  } > "$REPORT_FILE"

  ENV_FILE="$env_file" RUN_ID="$RUN_ID_BASE" PORT_BASE="${PORT_BASE:-}" UB_SIM_PORT_NUM="$PORT_NUM" \
    "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null
  source "$env_file"
  cleanup_script="$CLEANUP_SCRIPT"

  for idx in {1..8}; do
    node_id="${NODE_IDS[$idx]}"
    guest_log="$run_dir/${node_id}_guest.log"
    if ! wait_for_log_pattern "$guest_log" "\\[run_demo\\] boot flow completed, dropping to shell" "$BOOT_WAIT_SECS"; then
      echo "shell gate timeout: $node_id" >&2
      result=1
      break
    fi
  done

  if [[ "$result" -eq 0 ]]; then
    for idx in {1..8}; do
      node_id="${NODE_IDS[$idx]}"
      guest_log="$run_dir/${node_id}_guest.log"
      qemu_log="$run_dir/${node_id}_qemu.log"
      validate_guest_log "$node_id" "${NODE_IPS[$idx]}" "$guest_log" || result=1
      validate_qemu_log "$node_id" "$qemu_log" || result=1
      [[ "$result" -eq 0 ]] || break
    done
  fi

  if [[ "$result" -eq 0 ]]; then
    validate_unique_fabric_identity "$run_dir" || result=1
  fi

  cleanup_env "$cleanup_script"

  if [[ "$result" -eq 0 ]]; then
    echo "result=PASS" | tee -a "$REPORT_FILE"
    echo "run_dir=$run_dir" | tee -a "$REPORT_FILE"
    return 0
  fi

  echo "result=FAIL" | tee -a "$REPORT_FILE"
  echo "run_dir=$run_dir" | tee -a "$REPORT_FILE"
  return 1
}

main "$@"
