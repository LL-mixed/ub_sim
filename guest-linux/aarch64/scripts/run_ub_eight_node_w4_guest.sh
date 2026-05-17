#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
SIM_UAPI_W5_PROFILE="${SIM_UAPI_W5_PROFILE:-}"

w5_profile_default_w4_backend() {
  case "$1" in
    ""|qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      echo qwen3_dense
      ;;
    *)
      echo ""
      ;;
  esac
}

w5_profile_default_engram() {
  case "$1" in
    qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      echo 1
      ;;
    *)
      echo 0
      ;;
  esac
}

if [[ -n "$SIM_UAPI_W5_PROFILE" && -z "$(w5_profile_default_w4_backend "$SIM_UAPI_W5_PROFILE")" ]]; then
  echo "unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE" >&2
  exit 2
fi

if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w5_inference_cluster.trace.latest.txt}"
  RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE}_${RANDOM}}"
else
  TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w4_guest.trace.latest.txt}"
  RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w4guest8_${RANDOM}}"
fi
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w5_inference_cluster_summary.${RUN_ID_BASE}.txt}"
else
  RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w4_guest_summary.${RUN_ID_BASE}.txt}"
fi
RUN_INITRAMFS_DIR="$OUT_DIR/initramfs.${RUN_ID_BASE}"
RUN_INITRAMFS_IMAGE="$OUT_DIR/initramfs.${RUN_ID_BASE}.cpio.gz"
# Use a short unique suffix for the shared dir to stay under macOS 104-byte UNIX socket path limit.
_SHORT_SHARED_SUFFIX="$(printf '%s' "$RUN_ID_BASE" | cksum | cut -d' ' -f1)_${RANDOM}"
UB_FM_SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ubqe_${_SHORT_SHARED_SUFFIX}}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
DEMO_WAIT_SECS="${DEMO_WAIT_SECS:-600}"
W4_GUEST_PROGRESS_INTERVAL_SECS="${W4_GUEST_PROGRESS_INTERVAL_SECS:-180}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
QEMU_MEM="${QEMU_MEM:-8G}"
PORT_NUM="${UB_SIM_PORT_NUM:-7}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST="${SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST:-/tmp/simpler-host-engram-context-artifacts/host_engram_context_manifest.json}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-$(w5_profile_default_w4_backend "$SIM_UAPI_W5_PROFILE")}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}"
SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS="${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-9707,1207,16948,18}"
SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-$(w5_profile_default_engram "$SIM_UAPI_W5_PROFILE")}"
SIM_QWEN3_GUEST_ENGRAM_MODE="${SIM_QWEN3_GUEST_ENGRAM_MODE:-cpu}"
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE="${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE:-8}"
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE="${SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE:-0}"
SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI="${SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI:-1000}"
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW="${SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW:-0}"
SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS="${SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-disabled}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF:-}"
SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF:-}"
SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}"
SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="${SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS:-}"
SIM_W4_UAPI_COMPLETION_TIMEOUT_MS="${SIM_W4_UAPI_COMPLETION_TIMEOUT_MS:-900000}"
SIM_W4_RESOURCE_ASSERTIONS="${SIM_W4_RESOURCE_ASSERTIONS:-0}"
FATAL_GUEST_PATTERN="rcu_preempt|RCU grace-period|self-detected stall|detected stalls on CPUs/tasks|rx msg plen invalid|poller rx msg failed, ret=-22|timeout waiting completions|qwen3 .*missing|qwen3 .*mismatch|\\[w4_guest\\] fail"
FATAL_QEMU_PATTERN="SIM_DEC: cpu read failed|ub_link write failed|bounded write timed out|rx msg plen invalid|poller rx msg failed"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
ALL_IPS_CSV="${(j:,:)NODE_IPS}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

append_kernel_arg_if_missing() {
  local arg="$1"
  local key="${arg%%=*}"

  if [[ "$APPEND_BASE" != *"$key="* ]]; then
    APPEND_BASE="${APPEND_BASE} ${arg}"
  fi
}

append_kernel_arg_if_missing "pmd_mapping=25%"
append_kernel_arg_if_missing "obmm.skip_cache_maintain=1"
append_kernel_arg_if_missing "rcupdate.rcu_cpu_stall_timeout=300"

trace() {
  local msg="$1"
  printf '[w4guest8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
}

is_qwen3_dense_profile() {
  local profile="$1"
  [[ "$profile" == "qwen3_dense_reference" || "$profile" == "qwen3_dense" ]]
}

validate_w5_profile_runtime() {
  case "$SIM_UAPI_W5_PROFILE" in
    "")
      return 0
      ;;
    qwen3_0_6b_decode|qwen3_0_6b_engram_decode)
      if [[ "${SIM_QWEN3_DENSE_MODEL_KEY:-}" == "qwen3-14b" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires Qwen3-0.6B-compatible weights, got model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-unknown}"
        return 1
      fi
      ;;
    qwen3_14b_decode|qwen3_14b_engram_decode)
      if [[ "${SIM_QWEN3_DENSE_MODEL_KEY:-}" != "qwen3-14b" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires Qwen3-14B weights, got model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-unknown}"
        return 1
      fi
      ;;
    *)
      trace "FAIL: unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE"
      return 1
      ;;
  esac

  case "$SIM_UAPI_W5_PROFILE" in
    qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" != "1" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires SIM_QWEN3_GUEST_ENGRAM=1"
        return 1
      fi
      ;;
    qwen3_0_6b_decode|qwen3_14b_decode)
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        trace "FAIL: SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE requires SIM_QWEN3_GUEST_ENGRAM=0"
        return 1
      fi
      ;;
  esac
  return 0
}

validate_qwen3_weights_path() {
  if ! is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    return 0
  fi
  local weights_path="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  local required

  if [[ -z "$weights_path" ]]; then
    trace "FAIL: $SIM_UAPI_W4_CHIPBACKEND_PROFILE requires SIM_QWEN3_DENSE_WEIGHTS_PATH"
    return 1
  fi
  if [[ ! -d "$weights_path" ]]; then
    trace "FAIL: qwen3 weights path is not a directory path=$weights_path"
    return 1
  fi
  for required in config.json tokenizer.json; do
    if [[ ! -f "$weights_path/$required" ]]; then
      trace "FAIL: qwen3 weights path missing $required path=$weights_path"
      return 1
    fi
  done
  if [[ ! -f "$weights_path/model.safetensors" && ! -f "$weights_path/model.safetensors.index.json" ]]; then
    trace "FAIL: qwen3 weights path missing model.safetensors or model.safetensors.index.json path=$weights_path"
    return 1
  fi
  trace "prepare: qwen3 weights path ok path=$weights_path"
  return 0
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
  local pass_count="${6:-1}"
  local deadline=$((SECONDS + timeout_s))
  local tmp
  local count

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      count=0
      if [[ -n "$tmp" ]]; then
        count="$(printf '%s\n' "$tmp" | rg -c "$pass_pattern" || true)"
      fi
      if (( count >= pass_count )); then
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

wait_for_all_logs_pass_or_fail_since() {
  local pass_pattern="$1"
  local fail_pattern="$2"
  local timeout_s="$3"
  local pass_count="${4:-1}"
  local deadline=$((SECONDS + timeout_s))
  local wait_start="$SECONDS"
  local progress_interval="$W4_GUEST_PROGRESS_INTERVAL_SECS"
  local next_progress=0
  local node_id guest_log start_line tmp count all_pass

  if [[ ! "$progress_interval" =~ '^[0-9]+$' ]]; then
    trace "progress: invalid W4_GUEST_PROGRESS_INTERVAL_SECS=$progress_interval; using 180"
    progress_interval=180
  fi
  if (( progress_interval > 0 )); then
    trace "progress: reporting_interval_s=$progress_interval expected_decode_steps=$pass_count"
    next_progress=$((SECONDS + progress_interval))
  else
    trace "progress: reporting disabled W4_GUEST_PROGRESS_INTERVAL_SECS=0"
  fi

  while (( SECONDS < deadline )); do
    all_pass=1
    for node_id in "${NODE_IDS[@]}"; do
      guest_log="$RUN_DIR/${node_id}_guest.log"
      start_line="${START_LINES[$node_id]}"
      count=0
      if [[ -f "$guest_log" ]]; then
        tmp="$(tail -n "+$((start_line + 1))" "$guest_log" 2>/dev/null || true)"
        if [[ -n "$tmp" ]]; then
          if printf '%s\n' "$tmp" | rg -q "$fail_pattern"; then
            trace "FAIL: w4 guest fatal marker on $node_id"
            printf '%s\n' "$tmp" | rg "$fail_pattern" | tail -n 5 >&2 || true
            return 1
          fi
          count="$(printf '%s\n' "$tmp" | rg -c "$pass_pattern" || true)"
        fi
      fi
      if (( count < pass_count )); then
        all_pass=0
      fi
    done
    if (( all_pass )); then
      return 0
    fi
    if (( progress_interval > 0 && SECONDS >= next_progress )); then
      emit_w4_wait_progress "$wait_start" "$pass_count" || true
      next_progress=$((SECONDS + progress_interval))
    fi
    sleep 0.2
  done
  return 2
}

emit_w4_wait_progress() {
  local wait_start="$1"
  local pass_count="$2"
  local elapsed_s=$((SECONDS - wait_start))
  local line
  local summary_parser="$SCRIPT_DIR/w4_guest_run_summary.py"

  if [[ -n "$SIM_UAPI_W5_PROFILE" && -f "$SCRIPT_DIR/w5_inference_cluster_summary.py" ]]; then
    summary_parser="$SCRIPT_DIR/w5_inference_cluster_summary.py"
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    trace "progress: unavailable reason=python3_not_found"
    return 1
  fi

  if ! python3 "$summary_parser" \
    --progress "$RUN_DIR" "$pass_count" "$elapsed_s" "${NODE_IDS[@]}" | while IFS= read -r line; do
      trace "$line"
    done; then
    trace "progress: unavailable reason=progress_parser_failed"
    return 1
  fi
  return 0
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

assert_no_fatal_runtime_logs() {
  local node_id guest_log qemu_log

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    qemu_log="$RUN_DIR/${node_id}_qemu.log"
    assert_log_absent "$guest_log" "$FATAL_GUEST_PATTERN" "$node_id fatal guest runtime marker" || return 1
    assert_log_absent "$qemu_log" "$FATAL_QEMU_PATTERN" "$node_id fatal qemu runtime marker" || return 1
  done
  return 0
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

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

trace_run_artifact_paths() {
  local node_id

  if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
    trace "w5_profile: $SIM_UAPI_W5_PROFILE"
  fi
  trace "run_dir: $RUN_DIR"
  trace "control_log: $RUN_DIR/control.log"
  trace "cleanup_script: ${CLEANUP_SCRIPT:-}"
  trace "summary_file: $RUN_SUMMARY_FILE"
  for node_id in "${NODE_IDS[@]}"; do
    trace "${node_id}_guest_log: $RUN_DIR/${node_id}_guest.log"
    trace "${node_id}_qemu_log: $RUN_DIR/${node_id}_qemu.log"
  done
}

write_w4_initramfs_runner() {
  local runner="$RUN_INITRAMFS_DIR/bin/run_demo"

  cat > "$runner" <<EOF
#!/bin/busybox sh
set -u

log() {
  echo "[w4guest8:initramfs] \$*"
}

mount_fs() {
  local fstype="\$1"
  local target="\$2"
  /bin/busybox mkdir -p "\$target" >/dev/null 2>&1 || true
  /bin/busybox mount -t "\$fstype" none "\$target" >/dev/null 2>&1 || true
}

bootstrap_fs() {
  mount_fs proc /proc
  mount_fs sysfs /sys
  mount_fs devtmpfs /dev
  mount_fs devpts /dev/pts
}

cmdline_value() {
  local key="\$1"
  local arg
  for arg in \$(/bin/busybox cat /proc/cmdline 2>/dev/null || true); do
    case "\$arg" in
      "\$key"=*)
        echo "\${arg#*=}"
        return 0
        ;;
    esac
  done
  return 1
}

enter_shell() {
  if [ -x /bin/busybox ]; then
    exec /bin/busybox sh
  fi
  if [ -x /bin/sh ]; then
    exec /bin/sh
  fi
  return 1
}

node_role_from_ip() {
  case "\$1" in
    10.0.0.1) echo nodeA ;;
    10.0.0.2) echo nodeB ;;
    10.0.0.3) echo nodeC ;;
    10.0.0.4) echo nodeD ;;
    10.0.0.5) echo nodeE ;;
    10.0.0.6) echo nodeF ;;
    10.0.0.7) echo nodeG ;;
    10.0.0.8) echo nodeH ;;
    *) return 1 ;;
  esac
}

bootstrap_fs

if [ "\${UB_RUN_DEMO_FROM_INIT:-0}" != "1" ] && [ "\${1-}" != "--resume" ]; then
  log "bootstrap phase: launching /bin/linqu_init"
  UB_RUN_DEMO_FROM_INIT=1
  export UB_RUN_DEMO_FROM_INIT
  exec /bin/linqu_init "\$@"
fi

if [ "\${1-}" = "--resume" ]; then
  shift
fi

LINQU_UB_LOCAL_IP="\$(cmdline_value linqu_ipourma_ipv4 || true)"
if [ -z "\$LINQU_UB_LOCAL_IP" ]; then
  log "FAIL: missing linqu_ipourma_ipv4 on kernel cmdline"
  enter_shell
fi

LINQU_UB_ROLE="\$(node_role_from_ip "\$LINQU_UB_LOCAL_IP" || true)"
if [ -z "\$LINQU_UB_ROLE" ]; then
  log "FAIL: unknown linqu_ipourma_ipv4=\$LINQU_UB_LOCAL_IP"
  enter_shell
fi

export LINQU_UB_ROLE
export LINQU_UB_LOCAL_IP
export LINQU_UB_ALL_IPS="$ALL_IPS_CSV"
export LINQU_UB_NODE_COUNT=8
export LINQU_W4_DB_CLUSTER=1
export LINQU_W4_REQUIRE_UAPI_RESOURCE=1
export SIM_W4_DB_LAZY_REMOTE_ACTIVATION=1
export SIM_UAPI_W5_PROFILE="$SIM_UAPI_W5_PROFILE"
export SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE"
export SIM_QWEN3_DENSE_MODEL_ID="${SIM_QWEN3_DENSE_MODEL_ID:-}"
export SIM_QWEN3_DENSE_MODEL_KEY="${SIM_QWEN3_DENSE_MODEL_KEY:-}"
export SIM_QWEN3_DENSE_VOCAB_SIZE="${SIM_QWEN3_DENSE_VOCAB_SIZE:-}"
export SIM_QWEN3_DENSE_HIDDEN_SIZE="${SIM_QWEN3_DENSE_HIDDEN_SIZE:-}"
export SIM_QWEN3_DENSE_INTERMEDIATE_SIZE="${SIM_QWEN3_DENSE_INTERMEDIATE_SIZE:-}"
export SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS="${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-}"
export SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS="${SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS:-}"
export SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS="${SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS:-}"
export SIM_QWEN3_DENSE_HEAD_DIM="${SIM_QWEN3_DENSE_HEAD_DIM:-}"
export SIM_QWEN3_DENSE_PREFILL_TOKENS="${SIM_QWEN3_DENSE_PREFILL_TOKENS:-}"
export SIM_QWEN3_DENSE_DECODE_TOKENS="${SIM_QWEN3_DENSE_DECODE_TOKENS:-}"
export SIM_QWEN3_DENSE_TP_NODES="${SIM_QWEN3_DENSE_TP_NODES:-}"
export SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES="${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-}"
export SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES="${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}"
export SIM_QWEN3_DENSE_KV_STATE_BYTES="${SIM_QWEN3_DENSE_KV_STATE_BYTES:-}"
export SIM_QWEN3_GUEST_DECODE_STEP=0
export SIM_QWEN3_GUEST_DECODE_STEPS="$SIM_QWEN3_GUEST_DECODE_STEPS"
export SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS="$SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS"
export SIM_QWEN3_GUEST_ENGRAM="$SIM_QWEN3_GUEST_ENGRAM"
export SIM_QWEN3_GUEST_ENGRAM_MODE="$SIM_QWEN3_GUEST_ENGRAM_MODE"
export SIM_QWEN3_GUEST_ENGRAM_SESSION_ID="${SIM_QWEN3_GUEST_ENGRAM_SESSION_ID:-guest}"
export SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE="$SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE"
export SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE="$SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE"
export SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI="$SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI"
export SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW="$SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW"
export SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS="$SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF"
export SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF"
export SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR"
export SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS"
export SIM_W4_UAPI_COMPLETION_TIMEOUT_MS="$SIM_W4_UAPI_COMPLETION_TIMEOUT_MS"
export SIM_W4_RESOURCE_ASSERTIONS="$SIM_W4_RESOURCE_ASSERTIONS"

log "start step=0 \$LINQU_UB_ROLE local_ip=\$LINQU_UB_LOCAL_IP"
if /bin/linqu_w4_guest; then
  log "linqu_w4_guest completed \$LINQU_UB_ROLE"
else
  rc=\$?
  log "FAIL: linqu_w4_guest failed \$LINQU_UB_ROLE rc=\$rc"
fi

log "entering shell after w4 guest runner"
enter_shell
EOF
  chmod +x "$runner"
}

build_w4_initramfs() {
  local base_initramfs="$OUT_DIR/initramfs.cpio.gz"

  trace "prepare: build per-run initramfs image=$RUN_INITRAMFS_IMAGE"
  ensure_ub_guest_artifacts "$ROOT_DIR" "$OUT_DIR/Image" "$base_initramfs"
  rm -rf "$RUN_INITRAMFS_DIR" "$RUN_INITRAMFS_IMAGE"
  mkdir -p "$RUN_INITRAMFS_DIR"
  (
    cd "$RUN_INITRAMFS_DIR"
    gzip -dc "$base_initramfs" | cpio -id --quiet
  )
  write_w4_initramfs_runner
  (
    cd "$RUN_INITRAMFS_DIR"
    find . -print | cpio -o -H newc --quiet | gzip -9 > "$RUN_INITRAMFS_IMAGE"
  )
}

append_run_artifact_cleanup() {
  if [[ -n "${CLEANUP_SCRIPT:-}" && -f "$CLEANUP_SCRIPT" ]]; then
    cat >> "$CLEANUP_SCRIPT" <<EOF
rm -rf '$RUN_INITRAMFS_DIR'
rm -f '$RUN_INITRAMFS_IMAGE'
EOF
  fi
}

validate_owner_observed() {
  local node_id="$1"
  local log_file="$2"
  local owner_idx="$3"
  local owner_role="$4"

  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=prefix_group key=request/w4-${owner_role}-request-0/prefix-group/${owner_role}-group-0 group=${owner_role}-group-0 members=2 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} group" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=request_prefix key=request/w4-${owner_role}-request-0/prefix/${owner_role}-prefix-0 version=[0-9]+" "$node_id saw ${owner_role} prefix" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-0 state=hot version=[0-9]+" "$node_id saw ${owner_role} block0" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-1 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} block1" || return 1
}

qwen3_engram_context_refs_configured() {
  [[ -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF" &&
    -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF" &&
    -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF" ]]
}

qwen3_engram_context_op_enabled() {
  [[ -n "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" &&
    "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" != "disabled" &&
    "$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" != "none" ]]
}

validate_qwen3_engram_context_refs() {
  if [[ "$SIM_QWEN3_GUEST_ENGRAM" != "1" ]] || ! qwen3_engram_context_op_enabled; then
    return 0
  fi
  if ! qwen3_engram_context_refs_configured; then
    trace "FAIL: qwen3 engram context op requires object refs context_op=$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP hint=materialize Lingqu Memory Service context objects and export all SIM_QWEN3_GUEST_ENGRAM_CONTEXT_*_REF vars"
    return 1
  fi
  return 0
}

validate_w5_engram_context_summary() {
  if ! qwen3_engram_context_refs_configured; then
    return 0
  fi
  if [[ ! -f "$RUN_SUMMARY_FILE" ]]; then
    trace "FAIL: qwen3 engram context object-ref summary missing path=$RUN_SUMMARY_FILE"
    return 1
  fi
  if ! rg -q "engram_context_summary: records=${SIM_QWEN3_GUEST_DECODE_STEPS} steps=${SIM_QWEN3_GUEST_DECODE_STEPS}/${SIM_QWEN3_GUEST_DECODE_STEPS} modes=[^ ]*object-ref" "$RUN_SUMMARY_FILE"; then
    trace "FAIL: qwen3 engram context summary missing object-ref mode path=$RUN_SUMMARY_FILE"
    return 1
  fi
  return 0
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local expected_dispatch_word="0x41a0000041a00000"

  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "host_matmul" ]]; then
    expected_dispatch_word="0x3f8000003f800000"
  elif is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    expected_dispatch_word="0x[0-9a-f]+"
  fi

  local idx owner_role
  local remote_idx

  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_kvcache_path=ready" "$node_id obmm kvcache backing" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_cluster_mode=resource_backed_uapi" "$node_id db cluster resource-backed mode" || return 1
  idx="$(node_index "$node_id")"
  remote_idx=$((idx % 8 + 1))
  if [[ "$SIM_W4_RESOURCE_ASSERTIONS" == "1" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster=resource_backed_assertions_(ok|skipped) nodes=8 .*" "$node_id resource-backed db cluster assertions" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=weight_tile key=weights/qwen3[-.0-9a-z]*/node${idx}/tile0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm weight publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=kvcache_block key=kvcache/w4/node${idx}/block0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm kvcache publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=hidden_range_input key=hidden/qwen3[-.0-9a-z]*/node${idx}/range-input owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range input publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=hidden_range_output key=hidden/qwen3[-.0-9a-z]*/node${idx}/range-output owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_placement local=node${idx} key=placement/qwen3[-.0-9a-z]*/layer-range/node${idx} .* next=node${remote_idx} .* source=db_metadata strategy=balanced_layers status=ok" "$node_id qwen3 range forward placement" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_contract local=node${idx} .* pipeline_nodes=8 total_layers=[1-9][0-9]* .* balanced=true .*placement_source=db_metadata .*backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_object_desc_put local=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor put" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_object_desc_get remote=node${remote_idx} reader=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor get" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=weight_tile key=weights/qwen3[-.0-9a-z]*/node${remote_idx}/tile0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote weight resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=kvcache_block key=kvcache/w4/node${remote_idx}/block0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote kvcache resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=hidden_range_input key=hidden/qwen3[-.0-9a-z]*/node${remote_idx}/range-input owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range input resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=hidden_range_output key=hidden/qwen3[-.0-9a-z]*/node${remote_idx}/range-output owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range output resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_handoff local=node${idx} next=node${remote_idx} .* placement_source=db_metadata backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 range forward handoff" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_summary local=node${idx} nodes=8 layers=[1-9][0-9]* .* hidden_bytes=[1-9][0-9]* objects=2 .* balanced=true placement_source=db_metadata backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward summary" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0=payload_backing_resolved local=node${idx} remote=node${remote_idx} objects=4 bytes=8192 hidden_bytes=[1-9][0-9]* boundary_offsets=0,248,256,4088,4096 backing=obmm_pool metadata=db status=ok" "$node_id obmm payload backing resolved" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=open_resource ok path=" "$node_id uapi resource opened" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_endpoint ok" "$node_id uapi endpoint mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_queues ok" "$node_id uapi queues mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=queue_phys ok" "$node_id uapi queue phys" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=read_default_segment ok segment=[0-9]+" "$node_id uapi default segment" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_seeded segment=[0-9]+ bytes=8192 checksum=0x[0-9a-f]+" "$node_id uapi kvcache payload seeded" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_boundaries segment=[0-9]+ offsets=0,248,256,4088,4096,4104 status=ok" "$node_id uapi kvcache payload boundaries" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=128 puts=1 gets=1 role=hot_shared" "$node_id uapi kvcache shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=8192 puts=1 gets=1 role=legacy_demo_payload" "$node_id uapi kvcache boundary shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]*" "$node_id uapi kvcache db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* role=aux_block" "$node_id uapi kvcache aux db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ writes=1 reads=1" "$node_id uapi kvcache block descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-1 segment=[0-9]+ writes=1 reads=1 role=aux_block_boundary" "$node_id uapi kvcache aux block descriptor" || return 1
  if is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_dispatch_descriptor node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) segment=[0-9]+ task_id=31 object_ref_table_offset=0x[0-9a-f]+ object_ref_count=[0-9]+ source=db_metadata status=ok" "$node_id qwen3 range dispatch descriptor" || return 1
  else
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_chipbackend_dispatch_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ task_id=31" "$node_id uapi chipbackend descriptor" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=doorbell ok slots=15" "$node_id uapi doorbell" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=wait_completions ok cq_tail=15" "$node_id uapi completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=decode_completions ok" "$node_id decode completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_dispatch_result segment=[0-9]+ word0=${expected_dispatch_word}" "$node_id dispatch payload result" || return 1
  if is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_compute_contract node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=8 total_layers=[1-9][0-9]* hidden_bytes=[1-9][0-9]* source=(dispatch_task|runtime_forward) output=(completion|metadata) status=ok" "$node_id qwen3 range compute contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_runtime_forward node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=8 total_layers=[1-9][0-9]* hidden_bytes=[1-9][0-9]* input_checksum=0x[0-9a-f]+ output_checksum=0x[0-9a-f]+ range_checksum=0x[0-9a-f]+ real_layers=[0-9]+ payload_offset=0x[0-9a-f]+ payload_bytes=[1-9][0-9]* kv_payload_offset=0x[0-9a-f]+ kv_payload_bytes=[1-9][0-9]* kv_payload_checksum=0x[0-9a-f]+ source=runtime_forward output=metadata status=ok" "$node_id qwen3 range runtime forward" || return 1
    if (( idx > 1 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_runtime_input_loaded node=${idx} layers=\\[[0-9]+,[0-9]+\\) input_offset=0x[0-9a-f]+ input_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* source=obmm_object_view target=uapi_object_ref materialize=sim_uapi_adapter status=ok" "$node_id qwen3 runtime range input loaded" || return 1
    fi
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]] && qwen3_engram_context_refs_configured; then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_context_object_refs local=${node_id} node=${idx} table_ref_chars=[1-9][0-9]* indices_ref_chars=[1-9][0-9]* gate_weight_ref_chars=[1-9][0-9]* registry_dir=.* source=env_contract target=sim_uapi status=ok" "$node_id qwen3 engram context refs configured" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_context_object_refs_loaded node=${idx} step=[0-9]+ refs=3 table_bytes=[1-9][0-9]* indices_bytes=[1-9][0-9]* gate_weight_bytes=[1-9][0-9]* table_checksum=0x[0-9a-f]+ indices_checksum=0x[0-9a-f]+ gate_weight_checksum=0x[0-9a-f]+ source=env_contract target=uapi_object_ref status=ok" "$node_id qwen3 engram context refs loaded" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_runtime_output_publish local=node${idx} step=[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ output_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ producer_publish_mono_ms=[0-9]+ producer_clock_offset_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_shmem metadata=lingqu_object_service queue=obmm_spsc status=ok" "$node_id qwen3 runtime range output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_worker_handoff_timing local=${node_id} step=[0-9]+ node=${idx} .* producer_to_input_found_mono_ms=-?[0-9]+ .* input_found_to_handoff_ms=[0-9]+ .* status=ok" "$node_id qwen3 worker handoff timing" || return 1
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_timing local=${node_id} step=[0-9]+ node=${idx} owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} candidate_publish_ms=[0-9]+ candidate_wait_ms=[0-9]+ policy_select_ms=[0-9]+ decision_publish_ms=[0-9]+ selected_wait_ms=[0-9]+ selected_writeback_ms=[0-9]+ history_state_wait_ms=[0-9]+ qwen3_range_publish_ms=[0-9]+ qwen3_range_input_wait_ms=[0-9]+ status=ok" "$node_id qwen3 engram timing" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_kv_state_publish local=node${idx} step=[0-9]+ key=kvcache/qwen3[-.0-9a-z]*/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ slot_bytes=[1-9][0-9]* block_bytes=[1-9][0-9]* blocks=[1-9][0-9]* reserved_bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_shmem metadata=lingqu_object_service status=ok" "$node_id qwen3 range kv state publish" || return 1
    if (( SIM_QWEN3_GUEST_DECODE_STEPS > 1 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_kv_state_resolve local=node${idx} step=[1-9][0-9]* previous_step=[0-9]+ key=kvcache/qwen3[-.0-9a-z]*/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ key_hash=0x[0-9a-f]+ version=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ validation=object_ref_metadata source=obmm_object_view backing=obmm_shmem metadata=lingqu_object_service target=mapped_view status=ok" "$node_id qwen3 range kv state resolve" || return 1
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        if (( idx == 1 || idx == 8 || idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_history_wait step=[0-9]+ object_key=qwen3/session/[^/]+/tokens/history owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=[0-9]+ history_tokens=[1-9][0-9]* bytes=[1-9][0-9]* checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram history wait" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_state_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/engram/state owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 history_tokens=[1-9][0-9]* selected_token=[0-9]+ history_checksum=0x[0-9a-f]+ blocked=[0-9]+ fallback=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ bytes=128 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram state wait" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_resolved step=[1-9][0-9]* previous_step=[0-9]+ selected_token=[0-9]+ history_tokens=[1-9][0-9]* history_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ target=next_round_input status=ok" "$node_id qwen3 engram state resolved" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_prompt_tokens_from_history tokens=[1-9][0-9]* source=engram_history_object target=uapi_segment status=ok" "$node_id qwen3 prompt tokens from history" || return 1
        else
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_skip step=[1-9][0-9]* local=${node_id} reason=range_worker_stateless status=ok" "$node_id qwen3 engram state skip" || return 1
        fi
      fi
    fi
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]] && (( idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_candidates_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk owner=node8 version=1 candidate_count=[1-9][0-9]* bytes=256 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram candidates wait" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_token_select local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ history_tokens=[1-9][0-9]* raw_token=[0-9]+ runner_up=[0-9]+ selected_token=[0-9]+ candidate_count=[1-9][0-9]* candidate2=[0-9]+ candidate3=[0-9]+ blocked=[0-9]+ fallback=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ no_repeat_ngram_size=[0-9]+ repetition_penalty_milli=[0-9]+ history_window=[0-9]+ candidate_checksum=0x[0-9a-f]+ source=guest_policy status=ok" "$node_id qwen3 engram token select" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_decision_publish local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ objects=3 history_tokens=[1-9][0-9]* selected_token=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ fallback=[0-9]+ blocked=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ history_key=qwen3/session/[^/]+/tokens/history history_version=[0-9]+ selected_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected state_key=qwen3/session/[^/]+/step/[0-9]+/engram/state history_checksum=0x[0-9a-f]+ selected_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram decision publish" || return 1
    fi
    if (( idx == 8 )); then
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_candidates_publish local=node8 step=[0-9]+ candidate_count=[1-9][0-9]* candidates_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk candidates_version=1 candidates_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram candidates publish" || return 1
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_selected_token_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 bytes=64 token=[0-9]+ checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram selected token wait" || return 1
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_selected_writeback local=node8 step=[0-9]+ selected_token=[0-9]+ source=engram_selected_object target=terminal_token_result status=ok" "$node_id qwen3 engram selected writeback" || return 1
      fi
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_terminal_token_result_publish local=node8 step=[0-9]+ token=[0-9]+ runner_up=[0-9]+ margin_milli=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ piece_word0=0x[0-9a-f]+ piece_word1=0x[0-9a-f]+ object_key=tokens/qwen3[-.0-9a-z]*/decode-step[0-9]+ offset=0x[0-9a-f]+ bytes=64 checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 terminal token result publish" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_forward_only object=range_hidden publish=0 resolve_remote=0 compute=0 storage=obmm_object metadata=db status=ok" "$node_id qwen3 range-only flow" || return 1
    if (( idx == 8 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_logits_sampling_table entries=[12] entry_words=(20|45) table_bytes=(160|360|720) vocab=[1-9][0-9]* sampled_distinct=[12] logits_checksum_nonzero=[12] text_checksum_nonzero=[12] real_logits=[01] status=ok" "$node_id qwen3 logits sampling table" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_token_text_table entries=[12] entry_words=8 table_bytes=(64|128) total_bytes=[1-9][0-9]* piece_bytes=9 policy_kind=2 policy_hash=0x[0-9a-f]+ packed_matches=[12] checksum_matches=[12] boundary_first=1 boundary_last=1 status=ok" "$node_id qwen3 token text table" || return 1
    else
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_logits_sampling_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id qwen3 logits sampling table skipped" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_token_text_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id qwen3 token text table skipped" || return 1
    fi
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] completion_sources chipbackend=[1-9][0-9]* shmem=[2-9][0-9]* dfs=[2-9][0-9]* db=[2-9][0-9]* block=[2-9][0-9]* guest_uapi=[0-9]+" "$node_id completion source coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] completion_status success=15 retryable=0 fatal=0" "$node_id completion status" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared" "$node_id uapi kvcache shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=8192 puts=1 gets=1 source=shmem_service role=legacy_demo_payload" "$node_id uapi kvcache boundary shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-0 writes=1 reads=1 source=block_service" "$node_id uapi kvcache block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-1 writes=1 reads=1 source=block_service role=aux_block_boundary" "$node_id uapi kvcache aux block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service" "$node_id uapi kvcache db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service role=aux_block" "$node_id uapi kvcache aux db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] assessment service_coverage=5/5 dispatch_path=ubc_entity_chipbackend kvcache_shmem_segment=[0-9]+ kvcache_block=w4-${node_id}-block-0 kvcache_db_key=block/w4-${node_id}-block-0 kvcache_db_bytes=[1-9][0-9]* complete=true" "$node_id service coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] dispatch path=ubc_entity_chipbackend" "$node_id chipbackend dispatch marker" || return 1
  assert_log_absent "$log_file" "observer_metadata_only" "$node_id no observer-only path" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
  assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
  return 0
}

run_w4_demo() {
  local decode_step="$1"
  local node_id guest_log rc
  typeset -A START_LINES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    START_LINES[$node_id]=0
    trace "wait w4 guest step=$decode_step on $node_id log=$guest_log"
  done

  rc=0
  wait_for_all_logs_pass_or_fail_since "^\\[w4_guest\\] pass\\r?$" "$FATAL_GUEST_PATTERN" \
    "$((DEMO_WAIT_SECS * SIM_QWEN3_GUEST_DECODE_STEPS))" \
    "$SIM_QWEN3_GUEST_DECODE_STEPS" || rc=$?
  if [[ "$rc" != "0" ]]; then
    trace "FAIL: w4 guest did not pass on all nodes rc=$rc"
    return 1
  fi

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    validate_node_log "$node_id" "$guest_log" || return 1
  done
  assert_no_fatal_runtime_logs || return 1
  return 0
}

emit_w4_run_summary() {
  local summary_tmp="$RUN_SUMMARY_FILE.tmp"
  local line
  local summary_parser="$SCRIPT_DIR/w4_guest_run_summary.py"

  if [[ -n "$SIM_UAPI_W5_PROFILE" && -f "$SCRIPT_DIR/w5_inference_cluster_summary.py" ]]; then
    summary_parser="$SCRIPT_DIR/w5_inference_cluster_summary.py"
  fi

  if ! command -v python3 >/dev/null 2>&1; then
    trace "summary: unavailable reason=python3_not_found"
    return 1
  fi

  if ! python3 "$summary_parser" \
    "$RUN_DIR" "$SIM_QWEN3_GUEST_DECODE_STEPS" "${NODE_IDS[@]}" > "$summary_tmp"; then
    rm -f "$summary_tmp"
    trace "summary: unavailable reason=summary_parser_failed"
    return 1
  fi

  mv "$summary_tmp" "$RUN_SUMMARY_FILE"
  while IFS= read -r line; do
    trace "$line"
  done < "$RUN_SUMMARY_FILE"
  trace "summary_file: $RUN_SUMMARY_FILE"
  return 0
}

prepare_environment() {
  local guest_log node_id control_log env_file

  mkdir -p "$RUN_DIR" "$OUT_DIR"
  : > "$TRACE_FILE"
  env_file="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"
  control_log="$RUN_DIR/control.log"
  validate_qwen3_weights_path || return 1
  qwen3_dense_apply_config_env
  validate_w5_profile_runtime || return 1
  validate_qwen3_engram_context_refs || return 1
  if is_qwen3_dense_profile "$SIM_UAPI_W4_CHIPBACKEND_PROFILE"; then
    if [[ -z "$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS" ]]; then
      SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$((DEMO_WAIT_SECS * 1000))"
    fi
    trace "prepare: qwen3 dense profile=$SIM_UAPI_W4_CHIPBACKEND_PROFILE model_id=${SIM_QWEN3_DENSE_MODEL_ID:-} model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-} layers=${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-} hidden_range_bytes=${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-} decode_hidden_bytes=${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}"
    trace "prepare: qwen3 decode round barrier timeout ms=$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS"
  fi
  build_w4_initramfs
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  ENV_FILE="$env_file" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" QEMU_MEM="$QEMU_MEM" UB_SIM_PORT_NUM="$PORT_NUM" \
    INITRAMFS_IMAGE="$RUN_INITRAMFS_IMAGE" RDINIT="/bin/run_demo" \
    UB_FM_SHARED_DIR="$UB_FM_SHARED_DIR" \
    SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
    SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST="$SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST" \
    SIM_UAPI_W5_PROFILE="$SIM_UAPI_W5_PROFILE" \
    SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
    SIM_QWEN3_DENSE_MODEL_ID="${SIM_QWEN3_DENSE_MODEL_ID:-}" \
    SIM_QWEN3_DENSE_MODEL_KEY="${SIM_QWEN3_DENSE_MODEL_KEY:-}" \
    SIM_QWEN3_DENSE_WEIGHTS_PATH="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" \
    SIM_QWEN3_DENSE_VOCAB_SIZE="${SIM_QWEN3_DENSE_VOCAB_SIZE:-}" \
    SIM_QWEN3_DENSE_HIDDEN_SIZE="${SIM_QWEN3_DENSE_HIDDEN_SIZE:-}" \
    SIM_QWEN3_DENSE_INTERMEDIATE_SIZE="${SIM_QWEN3_DENSE_INTERMEDIATE_SIZE:-}" \
    SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS="${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-}" \
    SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS="${SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS:-}" \
    SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS="${SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS:-}" \
    SIM_QWEN3_DENSE_HEAD_DIM="${SIM_QWEN3_DENSE_HEAD_DIM:-}" \
    SIM_QWEN3_DENSE_PREFILL_TOKENS="${SIM_QWEN3_DENSE_PREFILL_TOKENS:-}" \
    SIM_QWEN3_DENSE_DECODE_TOKENS="${SIM_QWEN3_DENSE_DECODE_TOKENS:-}" \
    SIM_QWEN3_DENSE_TP_NODES="${SIM_QWEN3_DENSE_TP_NODES:-}" \
    SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES="${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-}" \
    SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES="${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}" \
    SIM_QWEN3_DENSE_KV_STATE_BYTES="${SIM_QWEN3_DENSE_KV_STATE_BYTES:-}" \
    SIM_QWEN3_DENSE_WEIGHTS_PATH="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" \
    SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="$SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS" \
    SIM_QWEN3_GUEST_ENGRAM_MODE="$SIM_QWEN3_GUEST_ENGRAM_MODE" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF="$SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF" \
    SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR" \
    SIM_ENGRAM_SIMT_ARTIFACT_DIR="${SIM_ENGRAM_SIMT_ARTIFACT_DIR:-}" \
    SIM_ENGRAM_SIMT_SELECTED_SYMBOL="${SIM_ENGRAM_SIMT_SELECTED_SYMBOL:-}" \
    SIM_ENGRAM_SIMT_SELECTED_CASE="${SIM_ENGRAM_SIMT_SELECTED_CASE:-}" \
    SIM_ENGRAM_SIMT_BINARY_PATH="${SIM_ENGRAM_SIMT_BINARY_PATH:-}" \
    SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH="${SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH:-}" \
    "$SCRIPT_DIR/launch_ub_eight_node_headless.sh" >/dev/null
  if [[ ! -f "$env_file" ]]; then
    trace "FAIL: headless env file was not created path=$env_file"
    if [[ -f "$control_log" ]]; then
      trace "headless control log tail follows path=$control_log"
      tail -n 80 "$control_log" | sed 's/^/[w4guest8] control: /' | tee -a "$TRACE_FILE" >&2
    fi
    return 1
  fi
  source "$env_file"
  append_run_artifact_cleanup
  trace_run_artifact_paths

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "wait initramfs runner gate: $node_id"
    if ! wait_for_log_pattern "$guest_log" "\\[w4guest8:initramfs\\] start step=0 $node_id" "$BOOT_WAIT_SECS"; then
      trace "FAIL: initramfs runner gate timeout for $node_id"
      return 1
    fi
  done
  trace "initramfs runner gate ok for all eight nodes"
  return 0
}

main() {
  local exit_code=1
  local step

  if ! prepare_environment; then
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if ! run_w4_demo 0; then
    if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
      trace "FAIL: eight-node w5 inference cluster validation failed profile=$SIM_UAPI_W5_PROFILE"
    else
      trace "FAIL: eight-node w4 guest resource-backed uapi/chipbackend service coverage validation failed"
    fi
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if true; then
    exit_code=0
    emit_w4_run_summary || true
    if ! validate_w5_engram_context_summary; then
      [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
      exit 1
    fi
    if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
      trace "PASS: eight-node w5 inference cluster profile=$SIM_UAPI_W5_PROFILE"
      echo "eight-node w5 inference cluster validation passed"
    else
      trace "PASS: eight-node w4 guest resource-backed uapi/chipbackend service coverage validated"
      echo "eight-node w4 guest validation passed"
    fi
  fi

  [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
  exit "$exit_code"
}

main "$@"
