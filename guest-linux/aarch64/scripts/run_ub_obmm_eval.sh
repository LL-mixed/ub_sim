#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

NODE_COUNT=""
SCENARIO_CONFIG=""
REMOTE_MEMORY_MODEL_MANIFEST=""
ASYNC_LOAD_MODEL=""
OBMM_ASYNC_ARGS=""
RUN_ID=""
TIMEOUT_SEC=180
EXPECTED_OUTCOME="success"
ASYNC_LOAD_PRODUCER_CONSUMER=0
ASYNC_LOAD_PRODUCER_INDEX=0
ASYNC_LOAD_COMPLETION=patch

usage() {
  cat <<'EOF'
Usage: run_ub_obmm_eval.sh \
  --node-count 2|4|8 \
  --scenario-config PATH \
  --remote-memory-model-manifest PATH \
  --obmm-async-args "ARGS" \
  [--async-load-model SPEC] \
  [--run-id ID] [--timeout-sec N]

Runs one OBMM evaluation case on all nodes, validates one machine-readable
summary per guest, cleans up the exact QEMU processes from this run, and emits
one canonical OBMM_EVAL_SUMMARY from nodeA plus per-node evidence.
EOF
}

require_value() {
  local option="$1"
  local value="${2-}"

  if [[ -z "$value" ]]; then
    echo "$option requires a value" >&2
    exit 2
  fi
}

sha256_file() {
  local file_path="$1"

  if [[ -x /usr/bin/sha256sum ]]; then
    /usr/bin/sha256sum "$file_path" | awk '{print $1}'
  elif (( ${+commands[sha256sum]} )); then
    "${commands[sha256sum]}" "$file_path" | awk '{print $1}'
  elif [[ -x /usr/bin/shasum ]]; then
    /usr/bin/shasum -a 256 "$file_path" | awk '{print $1}'
  elif (( ${+commands[shasum]} )); then
    "${commands[shasum]}" -a 256 "$file_path" | awk '{print $1}'
  else
    echo "no SHA-256 utility is available" >&2
    return 1
  fi
}

while (( $# )); do
  case "$1" in
    --node-count)
      require_value "$1" "${2-}"
      NODE_COUNT="$2"
      shift 2
      ;;
    --remote-memory-model-manifest)
      require_value "$1" "${2-}"
      REMOTE_MEMORY_MODEL_MANIFEST="$2"
      shift 2
      ;;
    --scenario-config)
      require_value "$1" "${2-}"
      SCENARIO_CONFIG="$2"
      shift 2
      ;;
    --async-load-model)
      require_value "$1" "${2-}"
      ASYNC_LOAD_MODEL="$2"
      shift 2
      ;;
    --obmm-async-args)
      require_value "$1" "${2-}"
      OBMM_ASYNC_ARGS="$2"
      shift 2
      ;;
    --run-id)
      require_value "$1" "${2-}"
      RUN_ID="$2"
      shift 2
      ;;
    --timeout-sec)
      require_value "$1" "${2-}"
      TIMEOUT_SEC="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$NODE_COUNT" in
  2|4|8) ;;
  *)
    echo "--node-count must be 2, 4, or 8" >&2
    exit 2
    ;;
esac
if [[ -z "$REMOTE_MEMORY_MODEL_MANIFEST" ||
      ! -f "$REMOTE_MEMORY_MODEL_MANIFEST" ]]; then
  echo "remote-memory model manifest does not exist: $REMOTE_MEMORY_MODEL_MANIFEST" >&2
  exit 2
fi
if [[ -z "$SCENARIO_CONFIG" || ! -f "$SCENARIO_CONFIG" ]]; then
  echo "scenario config does not exist: $SCENARIO_CONFIG" >&2
  exit 2
fi
if [[ -z "$OBMM_ASYNC_ARGS" ]]; then
  echo "--obmm-async-args is required" >&2
  exit 2
fi
if [[ "$TIMEOUT_SEC" != <1-> ]]; then
  echo "--timeout-sec must be a positive integer" >&2
  exit 2
fi
if [[ -z "$RUN_ID" ]]; then
  RUN_ID="$(date +%Y-%m-%d_%H-%M-%S)_obmm_eval_${NODE_COUNT}_${RANDOM}"
fi
if [[ "$RUN_ID" == *[^A-Za-z0-9._-]* ]]; then
  echo "--run-id may contain only letters, digits, dot, underscore, and dash" >&2
  exit 2
fi

OBMM_SHARED_BASE="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-obmm-eval}"
mkdir -p "$OBMM_SHARED_BASE"
OBMM_RUN_SHARED_DIR="$(
  mktemp -d "$OBMM_SHARED_BASE/${RUN_ID}.XXXXXX"
)"
export UB_FM_SHARED_DIR="$OBMM_RUN_SHARED_DIR"

REMOTE_MEMORY_MODEL_MANIFEST="$(
  cd "$(dirname "$REMOTE_MEMORY_MODEL_MANIFEST")"
  pwd
)/$(basename "$REMOTE_MEMORY_MODEL_MANIFEST")"
SCENARIO_CONFIG="$(
  cd "$(dirname "$SCENARIO_CONFIG")"
  pwd
)/$(basename "$SCENARIO_CONFIG")"

APPEND_EXTRA="linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_probe_hold=1"
APPEND_EXTRA="$APPEND_EXTRA pmd_mapping=100% obmm.mempool_size=512M cma=64M"
APPEND_EXTRA="$APPEND_EXTRA linqu_obmm_async_coroutine=1"
APPEND_EXTRA="$APPEND_EXTRA linqu_node_count=$NODE_COUNT"

append_cmdline() {
  local token="$1"

  if [[ " $APPEND_EXTRA " != *" $token "* ]]; then
    APPEND_EXTRA="$APPEND_EXTRA $token"
  fi
}

typeset -a async_words
async_words=(${=OBMM_ASYNC_ARGS})
integer async_index=1
while (( async_index <= ${#async_words} )); do
  option="${async_words[$async_index]}"
  if [[ "$option" == "--verify" ]]; then
    append_cmdline "obmm_async_verify=1"
    async_index=$((async_index + 1))
    continue
  fi
  if [[ "$option" == "--async-load-producer-consumer" ]]; then
    ASYNC_LOAD_PRODUCER_CONSUMER=1
    append_cmdline "obmm_async_load_producer_consumer=1"
    async_index=$((async_index + 1))
    continue
  fi
  if (( async_index == ${#async_words} )); then
    echo "$option requires a value in --obmm-async-args" >&2
    exit 2
  fi
  value="${async_words[$((async_index + 1))]}"
  if [[ -z "$value" ]]; then
    echo "$option requires a value in --obmm-async-args" >&2
    exit 2
  fi
  case "$option" in
    --mode) append_cmdline "obmm_async_mode=$value" ;;
    --async-load-completion)
      case "$value" in
        patch|replay) ;;
        *)
          echo "--async-load-completion must be patch or replay" >&2
          exit 2
          ;;
      esac
      ASYNC_LOAD_COMPLETION="$value"
      append_cmdline "obmm_async_load_completion=$value"
      ;;
    --coroutines) append_cmdline "obmm_async_coroutines=$value" ;;
    --inflight) append_cmdline "obmm_async_inflight=$value" ;;
    --lookahead) append_cmdline "obmm_async_lookahead=$value" ;;
    --access-bytes) append_cmdline "obmm_async_access_bytes=$value" ;;
    --pattern) append_cmdline "obmm_async_pattern=$value" ;;
    --compute-us) append_cmdline "obmm_async_compute_us=$value" ;;
    --iterations) append_cmdline "obmm_async_iterations=$value" ;;
    --warmup) append_cmdline "obmm_async_warmup=$value" ;;
    --min-duration-ms) append_cmdline "obmm_async_min_duration_ms=$value" ;;
    --trace-sample-ppm) append_cmdline "obmm_async_trace_sample_ppm=$value" ;;
    --expected-outcome)
      EXPECTED_OUTCOME="$value"
      append_cmdline "obmm_async_expected_outcome=$value"
      ;;
    --deadline-us) append_cmdline "obmm_async_deadline_us=$value" ;;
    --seed) append_cmdline "obmm_async_seed=$value" ;;
    --peer-index) append_cmdline "obmm_async_peer_index=$value" ;;
    --producer-index)
      ASYNC_LOAD_PRODUCER_INDEX="$value"
      append_cmdline "obmm_async_producer_index=$value"
      ;;
    --uffd-case) append_cmdline "obmm_uffd_case=$value" ;;
    --worker-threads) append_cmdline "obmm_uffd_worker_threads=$value" ;;
    --handler-cpu) append_cmdline "obmm_uffd_handler_cpu=$value" ;;
    --pages) append_cmdline "obmm_uffd_pages=$value" ;;
    --case) append_cmdline "obmm_baseline_case=$value" ;;
    --eval-band) append_cmdline "obmm_eval_band=$value" ;;
    --eval-case) append_cmdline "obmm_eval_case=$value" ;;
    *)
      echo "unsupported --obmm-async-args option: $option" >&2
      exit 2
      ;;
  esac
  async_index=$((async_index + 2))
done

if (( ASYNC_LOAD_PRODUCER_CONSUMER )); then
  if [[ "$NODE_COUNT" != "2" || "$ASYNC_LOAD_PRODUCER_INDEX" != "0" ||
        "$OBMM_ASYNC_ARGS" != *"--mode async-load"* ||
        "$EXPECTED_OUTCOME" != "success" ]]; then
    echo "ASYNC_LOAD producer/consumer validation requires node_count=2, producer_index=0, async-load mode, and success outcome" >&2
    exit 2
  fi
fi

if [[ "$OBMM_ASYNC_ARGS" == *"--mode async-load"* &&
      -z "$ASYNC_LOAD_MODEL" ]]; then
  echo "async-load mode requires --async-load-model" >&2
  exit 2
fi

if [[ "$NODE_COUNT" == "4" ]]; then
  LAUNCHER="$SCRIPT_DIR/launch_ub_four_node_headless.sh"
  NODE_IDS=(nodeA nodeB nodeC nodeD)
else
  LAUNCHER="$SCRIPT_DIR/launch_ub_eight_node_headless.sh"
  if [[ "$NODE_COUNT" == "2" ]]; then
    NODE_IDS=(nodeA nodeB)
  else
    NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
  fi
  export SIM_W5_CLUSTER_NODE_COUNT="$NODE_COUNT"
fi

export APPEND_EXTRA
export REMOTE_MEMORY_MODEL_MANIFEST
export ASYNC_LOAD_MODEL
export RUN_ID
export SIM_UAPI_SCENARIO_CONFIG="$SCENARIO_CONFIG"

launcher_output="$(zsh "$LAUNCHER")"
print -r -- "$launcher_output"
ENV_FILE="${launcher_output##*$'\n'}"
if [[ ! -f "$ENV_FILE" ]]; then
  echo "launcher did not produce an environment file: $ENV_FILE" >&2
  exit 1
fi
source "$ENV_FILE"

for artifact in "$QEMU_BIN" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"; do
  if [[ ! -f "$artifact" ]]; then
    echo "launcher artifact is missing: $artifact" >&2
    exit 1
  fi
done
SCENARIO_SHA256="$(sha256_file "$SCENARIO_CONFIG")"
MODEL_FILE_SHA256="$(sha256_file "$REMOTE_MEMORY_MODEL_MANIFEST")"
QEMU_SHA256="$(sha256_file "$QEMU_BIN")"
KERNEL_SHA256="$(sha256_file "$KERNEL_IMAGE")"
INITRAMFS_SHA256="$(sha256_file "$INITRAMFS_IMAGE")"
MODEL_CONTRACT_HASH="$(
  sed -n 's/.*"manifest_hash"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
    "$REMOTE_MEMORY_MODEL_MANIFEST" | head -n 1
)"
if [[ -z "$MODEL_CONTRACT_HASH" ]]; then
  echo "model manifest does not contain manifest_hash" >&2
  exit 1
fi

cleaned=0
cleanup_run() {
  if (( cleaned )); then
    return
  fi
  cleaned=1
  if [[ -n "${CLEANUP_SCRIPT:-}" && -x "$CLEANUP_SCRIPT" ]]; then
    zsh "$CLEANUP_SCRIPT" >/dev/null 2>&1 || true
  fi
  if [[ -n "${OBMM_RUN_SHARED_DIR:-}" &&
        -d "$OBMM_RUN_SHARED_DIR" ]]; then
    rm -rf -- "$OBMM_RUN_SHARED_DIR"
  fi
}
trap cleanup_run EXIT INT TERM

summary_field() {
  local line="$1"
  local name="$2"
  local field

  for field in ${=line}; do
    if [[ "$field" == "$name="* ]]; then
      print -r -- "${field#*=}"
      return 0
    fi
  done
  return 1
}

deadline=$((SECONDS + TIMEOUT_SEC))
if (( ASYNC_LOAD_PRODUCER_CONSUMER )); then
  producer_log="$RUN_DIR/nodeA_guest.log"
  consumer_log="$RUN_DIR/nodeB_guest.log"
  while true; do
    producer_count=0
    consumer_count=0
    if [[ -f "$producer_log" ]]; then
      producer_count="$(grep -c '^OBMM_ASYNC_LOAD_EXPORT .*status=ready' "$producer_log" || true)"
    fi
    if [[ -f "$consumer_log" ]]; then
      consumer_count="$(grep -c '^OBMM_ASYNC_LOAD_SUMMARY .*status=pass' "$consumer_log" || true)"
    fi
    if (( producer_count == 1 && consumer_count == 1 )); then
      break
    fi
    if (( producer_count > 1 || consumer_count > 1 )); then
      echo "ASYNC_LOAD producer/consumer emitted duplicate terminal markers" >&2
      exit 1
    fi
    for node_id in nodeA nodeB; do
      pid_file="$ROOT_DIR/out/ub_${node_id}.headless.${RUN_ID}.pid"
      guest_log="$RUN_DIR/${node_id}_guest.log"
      if [[ -f "$pid_file" ]]; then
        qemu_pid="$(<"$pid_file")"
        if [[ -n "$qemu_pid" ]] && ! kill -0 "$qemu_pid" 2>/dev/null; then
          echo "$node_id QEMU exited before ASYNC_LOAD producer/consumer proof completed" >&2
          tail -n 80 "$guest_log" >&2 || true
          exit 1
        fi
      fi
    done
    if (( SECONDS >= deadline )); then
      echo "timeout waiting for ASYNC_LOAD producer/consumer proof" >&2
      tail -n 100 "$producer_log" >&2 || true
      tail -n 160 "$consumer_log" >&2 || true
      exit 1
    fi
    sleep 0.2
  done

  async_load_export="$(grep '^OBMM_ASYNC_LOAD_EXPORT .*status=ready' "$producer_log" | tr -d '\r')"
  async_load_summary="$(grep '^OBMM_ASYNC_LOAD_SUMMARY .*status=pass' "$consumer_log" | tr -d '\r')"
  async_load_coroutines="$(summary_field "$async_load_summary" coroutines)"
  async_load_export_mem_id="$(summary_field "$async_load_export" export_mem_id)"
  async_load_source_mem_id="$(summary_field "$async_load_summary" source_export_mem_id)"
  if [[ "$async_load_coroutines" != <2-> || "$async_load_source_mem_id" != "$async_load_export_mem_id" ||
        "$(summary_field "$async_load_summary" async_load_completion)" != "$ASYNC_LOAD_COMPLETION" ||
        "$(summary_field "$async_load_export" writes)" != "$async_load_coroutines" ||
        "$(summary_field "$async_load_summary" completed)" != "$async_load_coroutines" ||
        "$(summary_field "$async_load_summary" values_verified)" != "$async_load_coroutines" ||
        "$(summary_field "$async_load_summary" el0_upcalls_pending)" != "$async_load_coroutines" ||
        "$(summary_field "$async_load_summary" el0_upcalls_complete)" != "$async_load_coroutines" ||
        "$(summary_field "$async_load_summary" el0_upcalls_fault)" != "0" ||
        "$(summary_field "$async_load_summary" qemu_context_saves)" != "0" ||
        "$(summary_field "$async_load_summary" qemu_context_restores)" != "0" ||
        "$(summary_field "$async_load_summary" qemu_context_switches)" != "0" ||
        "$(summary_field "$async_load_summary" qemu_context_bytes)" != "0" ||
        "$(summary_field "$async_load_summary" async_load_pending_final)" != "0" ||
        "$(summary_field "$async_load_summary" backend_pending_final)" != "0" ||
        "$(summary_field "$async_load_summary" trace_dropped)" != "0" ||
        "$(summary_field "$async_load_summary" el0_context_switches)" != <1-> ]]; then
    echo "ASYNC_LOAD producer/consumer terminal summary is inconsistent" >&2
    exit 1
  fi
  case "$ASYNC_LOAD_COMPLETION" in
    patch)
      if [[ "$(summary_field "$async_load_summary" replay_consumed)" != "0" ||
            "$(summary_field "$async_load_summary" replay_mismatch)" != "0" ]]; then
        echo "ASYNC_LOAD patch mode reported replay activity" >&2
        exit 1
      fi
      ;;
    replay)
      if [[ "$(summary_field "$async_load_summary" replay_consumed)" != "$async_load_coroutines" ||
            "$(summary_field "$async_load_summary" replay_mismatch)" != "0" ||
            "$(summary_field "$async_load_summary" replay_ready_high_water)" != <1-> ]]; then
        echo "ASYNC_LOAD replay mode lacks exact-once retirement evidence" >&2
        exit 1
      fi
      ;;
  esac

  blocked_load_switches=0
  for (( coroutine_id = 0; coroutine_id < async_load_coroutines; coroutine_id++ )); do
    write_line="$(grep "^OBMM_ASYNC_LOAD_WRITE .*coroutine=${coroutine_id} " "$producer_log" | tr -d '\r')"
    context_line="$(grep "^OBMM_ASYNC_LOAD_CONTEXT .*coroutine=${coroutine_id} " "$consumer_log" | tr -d '\r')"
    issue_line="$(grep "^OBMM_ASYNC_LOAD_LDR .*event=issue coroutine=${coroutine_id} " "$consumer_log" | tr -d '\r')"
    pending_line="$(grep "^OBMM_ASYNC_LOAD_UPCALL .*event=pending coroutine=${coroutine_id} " "$consumer_log" | tr -d '\r')"
    complete_line="$(grep "^OBMM_ASYNC_LOAD_UPCALL .*event=complete coroutine=${coroutine_id} " "$consumer_log" | tr -d '\r')"
    resume_line="$(grep "^OBMM_ASYNC_LOAD_SCHEDULE .*to_coroutine=${coroutine_id} after_complete=1" "$consumer_log" | tr -d '\r')"
    retire_line="$(grep "^OBMM_ASYNC_LOAD_LDR .*event=retire coroutine=${coroutine_id} .*status=pass" "$consumer_log" | tr -d '\r')"
    coroutine_summary="$(grep "^OBMM_ASYNC_LOAD_COROUTINE_SUMMARY .*coroutine=${coroutine_id} .*status=pass" "$consumer_log" | tr -d '\r')"
    if [[ -z "$write_line" || -z "$context_line" || -z "$issue_line" ||
          -z "$pending_line" || -z "$complete_line" || -z "$resume_line" ||
          -z "$retire_line" || -z "$coroutine_summary" ]]; then
      echo "ASYNC_LOAD coroutine $coroutine_id is missing causal evidence" >&2
      exit 1
    fi
    context_id="$(summary_field "$context_line" context_id)"
    expected_value="$(summary_field "$write_line" value)"
    if [[ "$(summary_field "$write_line" export_mem_id)" != "$async_load_export_mem_id" ||
          "$(summary_field "$issue_line" context_id)" != "$context_id" ||
          "$(summary_field "$pending_line" context_id)" != "$context_id" ||
          "$(summary_field "$complete_line" context_id)" != "$context_id" ||
          "$(summary_field "$retire_line" context_id)" != "$context_id" ||
          "$(summary_field "$coroutine_summary" context_id)" != "$context_id" ||
          "$(summary_field "$issue_line" expected)" != "$expected_value" ||
          "$(summary_field "$complete_line" value)" != "$expected_value" ||
          "$(summary_field "$retire_line" actual)" != "$expected_value" ||
          "$(summary_field "$coroutine_summary" expected)" != "$expected_value" ||
          "$(summary_field "$coroutine_summary" actual)" != "$expected_value" ||
          "$(summary_field "$pending_line" token)" != "$(summary_field "$complete_line" token)" ||
          "$(summary_field "$pending_line" pc)" != "$(summary_field "$complete_line" pc)" ||
          "$(summary_field "$coroutine_summary" pending)" != "1" ||
          "$(summary_field "$coroutine_summary" complete)" != "1" ||
          "$(summary_field "$coroutine_summary" resumes_after_complete)" != <1-> ]]; then
      echo "ASYNC_LOAD coroutine $coroutine_id has inconsistent context, token, PC, or value evidence" >&2
      exit 1
    fi
    issue_position="$(grep -n "^OBMM_ASYNC_LOAD_LDR .*event=issue coroutine=${coroutine_id} " "$consumer_log" | cut -d: -f1)"
    pending_position="$(grep -n "^OBMM_ASYNC_LOAD_UPCALL .*event=pending coroutine=${coroutine_id} " "$consumer_log" | cut -d: -f1)"
    complete_position="$(grep -n "^OBMM_ASYNC_LOAD_UPCALL .*event=complete coroutine=${coroutine_id} " "$consumer_log" | cut -d: -f1)"
    resume_position="$(grep -n "^OBMM_ASYNC_LOAD_SCHEDULE .*to_coroutine=${coroutine_id} after_complete=1" "$consumer_log" | cut -d: -f1)"
    retire_position="$(grep -n "^OBMM_ASYNC_LOAD_LDR .*event=retire coroutine=${coroutine_id} " "$consumer_log" | cut -d: -f1)"
    if (( issue_position >= pending_position || pending_position >= complete_position ||
          complete_position >= resume_position || resume_position >= retire_position )); then
      echo "ASYNC_LOAD coroutine $coroutine_id causal event order is invalid" >&2
      exit 1
    fi
    switched_context=""
    switched_load=""
    if (( complete_position > pending_position + 1 )); then
      switched_context="$(sed -n "$((pending_position + 1)),$((complete_position - 1))p" "$consumer_log" | grep '^OBMM_ASYNC_LOAD_SCHEDULE .*event=resume ' | grep -v "to_context_id=${context_id} " | head -n 1 | tr -d '\r' || true)"
      switched_load="$(sed -n "$((pending_position + 1)),$((complete_position - 1))p" "$consumer_log" | grep '^OBMM_ASYNC_LOAD_LDR .*event=issue ' | grep -v "context_id=${context_id} " | head -n 1 | tr -d '\r' || true)"
    fi
    if [[ -n "$switched_context" && -n "$switched_load" ]]; then
      blocked_load_switches=$((blocked_load_switches + 1))
    fi
  done
  if (( blocked_load_switches == 0 )); then
    echo "ASYNC_LOAD evidence does not show a blocked load switching to another coroutine that issues its own LDR" >&2
    exit 1
  fi
else
  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    pid_file="$ROOT_DIR/out/ub_${node_id}.headless.${RUN_ID}.pid"
    while true; do
      summary_count=0
      if [[ -f "$guest_log" ]]; then
        summary_count="$(grep -c '^OBMM_EVAL_SUMMARY ' "$guest_log" || true)"
      fi
      if (( summary_count == 1 )); then
        break
      fi
      if (( summary_count > 1 )); then
        echo "$node_id emitted more than one OBMM_EVAL_SUMMARY" >&2
        exit 1
      fi
      if [[ -f "$pid_file" ]]; then
        qemu_pid="$(<"$pid_file")"
        if [[ -n "$qemu_pid" ]] && ! kill -0 "$qemu_pid" 2>/dev/null; then
          echo "$node_id QEMU exited before OBMM_EVAL_SUMMARY" >&2
          tail -n 80 "$guest_log" >&2 || true
          exit 1
        fi
      fi
      if (( SECONDS >= deadline )); then
        echo "timeout waiting for $node_id OBMM_EVAL_SUMMARY" >&2
        tail -n 80 "$guest_log" >&2 || true
        exit 1
      fi
      sleep 0.2
    done
  done

  typeset -a summaries
  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    summaries+=("$(grep '^OBMM_EVAL_SUMMARY ' "$guest_log" | tr -d '\r')")
  done

  for identity_field in case seed operations checksum failures timeouts fail_closed_process_exit status; do
    expected_value="$(summary_field "${summaries[1]}" "$identity_field")"
    for summary in "${summaries[@]}"; do
      actual_value="$(summary_field "$summary" "$identity_field")"
      if [[ "$actual_value" != "$expected_value" ]]; then
        echo "cross-node summary mismatch: field=$identity_field expected=$expected_value actual=$actual_value" >&2
        exit 1
      fi
    done
  done

  summary_status="$(summary_field "${summaries[1]}" status)"
  summary_operations="$(summary_field "${summaries[1]}" operations)"
  summary_failures="$(summary_field "${summaries[1]}" failures)"
  summary_timeouts="$(summary_field "${summaries[1]}" timeouts)"
  case "$EXPECTED_OUTCOME" in
    success|duplicate-late)
      if [[ "$summary_status" != "pass" || "$summary_operations" != <1-> ||
            "$summary_failures" != "0" || "$summary_timeouts" != "0" ]]; then
        echo "successful outcome did not complete exactly once: status=$summary_status operations=$summary_operations failures=$summary_failures timeouts=$summary_timeouts" >&2
        exit 1
      fi
      ;;
    error)
      if [[ "$summary_status" != "fail" || "$summary_failures" != <1-> ||
            "$summary_timeouts" != "0" ]]; then
        echo "error outcome did not fail closed: status=$summary_status failures=$summary_failures timeouts=$summary_timeouts" >&2
        exit 1
      fi
      ;;
    drop-timeout)
      if [[ "$summary_status" != "fail" || "$summary_failures" != <1-> ||
            "$summary_timeouts" != <1-> ]]; then
        echo "drop outcome did not become an explicit timeout: status=$summary_status failures=$summary_failures timeouts=$summary_timeouts" >&2
        exit 1
      fi
      ;;
    *)
      echo "unsupported expected outcome after launch: $EXPECTED_OUTCOME" >&2
      exit 1
      ;;
  esac
fi

for node_id in "${NODE_IDS[@]}"; do
  qemu_log="$RUN_DIR/${node_id}_qemu.log"
  if ! grep -q "manifest_hash=$MODEL_CONTRACT_HASH" "$qemu_log"; then
    echo "QEMU model manifest hash mismatch: node=$node_id expected=$MODEL_CONTRACT_HASH" >&2
    exit 1
  fi
done

typeset -a qemu_pids
for node_id in "${NODE_IDS[@]}"; do
  pid_file="$ROOT_DIR/out/ub_${node_id}.headless.${RUN_ID}.pid"
  if [[ ! -f "$pid_file" ]]; then
    echo "missing QEMU pid file before cleanup: $pid_file" >&2
    exit 1
  fi
  qemu_pids+=("$(<"$pid_file")")
done

cleanup_run
trap - EXIT INT TERM

integer pid_index=1
for node_id in "${NODE_IDS[@]}"; do
  qemu_pid="${qemu_pids[$pid_index]}"
  if [[ -n "$qemu_pid" ]] && kill -0 "$qemu_pid" 2>/dev/null; then
    echo "cleanup left QEMU running: node=$node_id pid=$qemu_pid" >&2
    exit 1
  fi
  pid_index=$((pid_index + 1))
done

integer node_index=1
print -r -- "OBMM_RUN_EVIDENCE node_count=$NODE_COUNT scenario_sha256=$SCENARIO_SHA256 model_file_sha256=$MODEL_FILE_SHA256 model_contract_hash=$MODEL_CONTRACT_HASH qemu_sha256=$QEMU_SHA256 kernel_sha256=$KERNEL_SHA256 initramfs_sha256=$INITRAMFS_SHA256 qemu_destroyed=1"
if (( ASYNC_LOAD_PRODUCER_CONSUMER )); then
  print -r -- "OBMM_ASYNC_LOAD_NODE_EVIDENCE node=nodeA role=producer export_mem_id=$async_load_export_mem_id writes=$async_load_coroutines status=ready"
  print -r -- "OBMM_ASYNC_LOAD_NODE_EVIDENCE node=nodeB role=consumer source_export_mem_id=$async_load_source_mem_id coroutines=$async_load_coroutines completed=$async_load_coroutines status=pass"
  print -r -- "OBMM_ASYNC_LOAD_CAUSAL_SUMMARY blocked_load_switches=$blocked_load_switches status=pass"
  grep '^OBMM_ASYNC_LOAD_\(WRITE\|EXPORT\)' "$producer_log"
  grep '^OBMM_ASYNC_LOAD_\(IMPORT\|CONTEXT\|LDR\|UPCALL\|SCHEDULE\|COROUTINE_SUMMARY\)' "$consumer_log"
  for node_id in nodeA nodeB; do
    qemu_log="$RUN_DIR/${node_id}_qemu.log"
    duplicate_count="$(grep -c 'duplicate=1' "$qemu_log" || true)"
    late_count="$(grep -c 'obmm_p1_late' "$qemu_log" || true)"
    print -r -- "OBMM_BACKEND_EVIDENCE node=$node_id duplicate=$duplicate_count late=$late_count drained=1"
  done
  print -r -- "$async_load_summary"
  exit 0
fi
for node_id in "${NODE_IDS[@]}"; do
  guest_log="$RUN_DIR/${node_id}_guest.log"
  qemu_log="$RUN_DIR/${node_id}_qemu.log"
  summary="${summaries[$node_index]}"
  print -r -- "OBMM_NODE_EVIDENCE node=$node_id drained=1 summary=${summary#OBMM_EVAL_SUMMARY }"
  grep '^OBMM_\(ASYNC\|UFFD\|BASELINE\)_SUMMARY ' "$guest_log" || true
  if (( node_index == 1 )); then
    grep '^OBMM_OPERATION_TRACE ' "$guest_log" || true
  fi
  duplicate_count="$(grep -c 'duplicate=1' "$qemu_log" || true)"
  late_count="$(grep -c 'obmm_p1_late' "$qemu_log" || true)"
  print -r -- "OBMM_BACKEND_EVIDENCE node=$node_id duplicate=$duplicate_count late=$late_count drained=1"
  node_index=$((node_index + 1))
done

print -r -- "${summaries[1]}"
