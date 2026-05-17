#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_demo}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
PORT_NUM="${UB_SIM_PORT_NUM:-7}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-eight}"
QMP_DIR="${SHARED_DIR}/qmp"
SERIAL_DIR="${SHARED_DIR}/serial"
MON_DIR="${SHARED_DIR}/mon"
UB_QEMU_RUNTIME_DIR="${UB_QEMU_RUNTIME_DIR:-${SHARED_DIR}/xdg_runtime}"
SIMPLER_HOST_VECTOR_MANIFEST="${SIMPLER_HOST_VECTOR_MANIFEST:-/tmp/simpler-host-vector-artifacts/host_vector_manifest.json}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST="${SIMPLER_HOST_ENGRAM_CONTEXT_MANIFEST:-/tmp/simpler-host-engram-context-artifacts/host_engram_context_manifest.json}"
SIM_UAPI_W5_PROFILE="${SIM_UAPI_W5_PROFILE:-}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-host_vector}"
SIM_UAPI_SCENARIO_CONFIG="${SIM_UAPI_SCENARIO_CONFIG:-$WORKSPACE_ROOT/scenarios/mvp_8host_single_domain.yaml}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_headless8_${RANDOM}}"
# macOS UNIX domain socket path limit is 104 bytes. Use a short suffix for socket file names.
SOCKET_SUFFIX="${SOCKET_SUFFIX:-$$_${RANDOM}}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
QEMU_MEM="${QEMU_MEM:-2G}"
QEMU_SMP="${QEMU_SMP:-2}"
CONTROL_LOG="$LOG_DIR/${RUN_ID}_headless8/control.log"
CLEANUP_SCRIPT="$OUT_DIR/headless_eight_node_cleanup.${RUN_ID}.sh"
ENV_FILE="${ENV_FILE:-$OUT_DIR/headless_eight_node_env.${RUN_ID}.sh}"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"

need_cmd() {
  local cmd="$1"
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "missing required command: $cmd" >&2
    exit 1
  fi
}

log() {
  echo "[headless8] $*" | tee -a "$CONTROL_LOG"
}

validate_qwen3_weights_path() {
  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" != "qwen3_dense_reference" && "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" != "qwen3_dense" ]]; then
    return 0
  fi
  local weights_path="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  local required

  if [[ -z "$weights_path" ]]; then
    echo "[headless8] $SIM_UAPI_W4_CHIPBACKEND_PROFILE requires SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    return 1
  fi
  if [[ ! -d "$weights_path" ]]; then
    echo "[headless8] qwen3 weights path is not a directory: $weights_path" >&2
    return 1
  fi
  for required in config.json tokenizer.json; do
    if [[ ! -f "$weights_path/$required" ]]; then
      echo "[headless8] qwen3 weights path missing $required in $weights_path" >&2
      return 1
    fi
  done
  if [[ ! -f "$weights_path/model.safetensors" && ! -f "$weights_path/model.safetensors.index.json" ]]; then
    echo "[headless8] qwen3 weights path missing model.safetensors or model.safetensors.index.json in $weights_path" >&2
    return 1
  fi
  return 0
}

wait_for_qemu_socket() {
  local node_id="$1"
  local qmp_socket="$2"
  local pid_file="$3"
  local max_wait_seconds="${4:-30}"
  local wait_interval_seconds=0.1
  local max_attempts=$(( max_wait_seconds * 10 ))
  local attempt=0
  local sleep_ms=100
  local sleep_seconds=$(awk "BEGIN { printf \"%.1f\", $sleep_ms / 1000.0 }")

  while (( attempt < max_attempts )); do
    if [[ -S "$qmp_socket" ]]; then
      return 0
    fi

    if [[ -f "$pid_file" ]]; then
      local pid
      pid="$(cat "$pid_file" 2>/dev/null || true)"
      if [[ -n "${pid:-}" ]] && ! kill -0 "$pid" 2>/dev/null; then
        log "qemu exited before QMP socket ready: node=$node_id pid=$pid qmp=$qmp_socket"
        return 1
      fi
    fi

    sleep "$sleep_seconds"
    attempt=$(( attempt + 1 ))
  done

  log "timeout waiting for QMP socket: node=$node_id qmp=$qmp_socket"
  return 1
}

cont_qemu() {
  local qmp_socket="$1"
  local attempt=0
  while (( attempt < 80 )); do
    if python3 - "$qmp_socket" <<'PY'
import socket
import sys
path = sys.argv[1]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(path)
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\r\n')
s.recv(4096)
s.sendall(b'{"execute":"cont"}\r\n')
s.recv(4096)
s.close()
PY
    then
      return 0
    fi
    sleep 0.2
    attempt=$((attempt + 1))
  done
  return 1
}

start_node() {
  local node_id="$1"
  local local_ip="$2"
  local mon_socket="$3"
  local serial_socket="$4"
  local qemu_log="$5"
  local guest_log="$6"
  local pid_file="$7"
  local qmp_socket="$8"
  local node_append_extra="$APPEND_EXTRA linqu_ipourma_ipv4=$local_ip"

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_SIM_PORT_NUM="$PORT_NUM" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    SIMPLER_HOST_VECTOR_MANIFEST="$SIMPLER_HOST_VECTOR_MANIFEST" \
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
    SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS="${SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS:-}" \
    SIM_QWEN3_GUEST_ENGRAM_MODE="${SIM_QWEN3_GUEST_ENGRAM_MODE:-cpu}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-disabled}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_ROWS:-}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_CHUNK_ELEMS:-}" \
    SIM_QWEN3_GUEST_ENGRAM_STATE_REF="${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_TABLE_REF:-}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_INDICES_REF:-}" \
    SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF="${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_GATE_WEIGHT_REF:-}" \
    SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}" \
    SIM_ENGRAM_SIMT_ARTIFACT_DIR="${SIM_ENGRAM_SIMT_ARTIFACT_DIR:-}" \
    SIM_ENGRAM_SIMT_SELECTED_SYMBOL="${SIM_ENGRAM_SIMT_SELECTED_SYMBOL:-}" \
    SIM_ENGRAM_SIMT_SELECTED_CASE="${SIM_ENGRAM_SIMT_SELECTED_CASE:-}" \
    SIM_ENGRAM_SIMT_BINARY_PATH="${SIM_ENGRAM_SIMT_BINARY_PATH:-}" \
    SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH="${SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH:-}" \
    SIM_UAPI_SCENARIO_CONFIG="$SIM_UAPI_SCENARIO_CONFIG" \
    XDG_RUNTIME_DIR="$UB_QEMU_RUNTIME_DIR" \
    "$QEMU_BIN" \
      -S \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -smp "$QEMU_SMP" \
      -m "$QEMU_MEM" \
      -nodefaults \
      -display none \
      -qmp unix:"$qmp_socket",server=on,wait=off \
      -chardev socket,id=mon0,path="$mon_socket",server=on,wait=off \
      -mon chardev=mon0,mode=readline \
      -chardev socket,id=ser0,path="$serial_socket",server=on,wait=off,logfile="$guest_log",logappend=on \
      -serial chardev:ser0 \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=${RDINIT} ${node_append_extra}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

need_cmd python3

mkdir -p "$OUT_DIR" "$LOG_DIR/${RUN_ID}_headless8" "$QMP_DIR" "$SERIAL_DIR" "$MON_DIR"
touch "$CONTROL_LOG"
validate_qwen3_weights_path
qwen3_dense_apply_config_env
if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_reference" || "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense" ]]; then
  log "qwen3_dense_profile=$SIM_UAPI_W4_CHIPBACKEND_PROFILE model_id=${SIM_QWEN3_DENSE_MODEL_ID:-} model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-} layers=${SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS:-} hidden_range_bytes=${SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES:-} decode_hidden_bytes=${SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES:-}"
fi

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ ! -f "$TOPOLOGY_FILE" ]]; then
  echo "TOPOLOGY_FILE not found: $TOPOLOGY_FILE" >&2
  exit 1
fi

cat > "$CLEANUP_SCRIPT" <<'EOC'
#!/bin/zsh
set -euo pipefail
NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
for node_id in "${NODE_IDS[@]}"; do
  pid_file="__OUT_DIR__/ub_${node_id}.headless.__RUN_ID__.pid"
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 0.2
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
  fi
  rm -f "__QMP_DIR__/${node_id}.__SOCKET_SUFFIX__.sock"
  rm -f "__SERIAL_DIR__/${node_id}.__SOCKET_SUFFIX__.sock"
  rm -f "__MON_DIR__/${node_id}.__SOCKET_SUFFIX__.sock"
done
rm -rf "__RUNTIME_DIR__"
rmdir "__QMP_DIR__" "__SERIAL_DIR__" "__MON_DIR__" 2>/dev/null || true
echo "cleaned run_id=__RUN_ID__"
EOC
perl -0pi -e 's#__OUT_DIR__#'"$OUT_DIR"'#g; s#__RUN_ID__#'"$RUN_ID"'#g; s#__SOCKET_SUFFIX__#'"$SOCKET_SUFFIX"'#g; s#__QMP_DIR__#'"$QMP_DIR"'#g; s#__SERIAL_DIR__#'"$SERIAL_DIR"'#g; s#__MON_DIR__#'"$MON_DIR"'#g; s#__RUNTIME_DIR__#'"$UB_QEMU_RUNTIME_DIR"'#g' "$CLEANUP_SCRIPT"
chmod +x "$CLEANUP_SCRIPT"

rm -rf "$UB_QEMU_RUNTIME_DIR"
mkdir -p "$UB_QEMU_RUNTIME_DIR"
rm -f "$QMP_DIR"/*.sock(N)
rm -f "$SERIAL_DIR"/*.sock(N)
rm -f "$MON_DIR"/*.sock(N)
touch "$CONTROL_LOG"

log "run_id=$RUN_ID"
log "qemu_bin=$QEMU_BIN"
log "qemu_mem=$QEMU_MEM"
log "qemu_smp=$QEMU_SMP"
log "topology=$TOPOLOGY_FILE"
log "append_extra=$APPEND_EXTRA"
log "ub_sim_port_num=$PORT_NUM"
if [[ -n "$SIM_UAPI_W5_PROFILE" ]]; then
  log "w5_profile=$SIM_UAPI_W5_PROFILE"
fi
if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_reference" || "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense" ]]; then
  log "qwen3_weights_path=${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  log "qwen3_model_id=${SIM_QWEN3_DENSE_MODEL_ID:-}"
  log "qwen3_model_key=${SIM_QWEN3_DENSE_MODEL_KEY:-}"
  log "qwen3_decode_round_barrier_timeout_ms=${SIM_QWEN3_DECODE_ROUND_BARRIER_TIMEOUT_MS:-}"
fi
log "logs_dir=$(dirname "$CONTROL_LOG")"

integer idx=0
for node_id in "${NODE_IDS[@]}"; do
  local_ip="${NODE_IPS[$((idx+1))]}"
  qemu_log="$(dirname "$CONTROL_LOG")/${node_id}_qemu.log"
  guest_log="$(dirname "$CONTROL_LOG")/${node_id}_guest.log"
  pid_file="$OUT_DIR/ub_${node_id}.headless.${RUN_ID}.pid"
  qmp_socket="$QMP_DIR/${node_id}.${SOCKET_SUFFIX}.sock"
  mon_socket="$MON_DIR/${node_id}.${SOCKET_SUFFIX}.sock"
  serial_socket="$SERIAL_DIR/${node_id}.${SOCKET_SUFFIX}.sock"

  log "starting ${node_id} local_ip=${local_ip} mon_socket=${mon_socket} serial_socket=${serial_socket}"
  start_node "$node_id" "$local_ip" "$mon_socket" "$serial_socket" "$qemu_log" "$guest_log" "$pid_file" "$qmp_socket"
  idx=$((idx + 1))
  sleep 0.2
done

log "waiting for QMP sockets"
for node_id in "${NODE_IDS[@]}"; do
  qmp_socket="$QMP_DIR/${node_id}.${SOCKET_SUFFIX}.sock"
  pid_file="$OUT_DIR/ub_${node_id}.headless.${RUN_ID}.pid"
  if ! wait_for_qemu_socket "$node_id" "$qmp_socket" "$pid_file" 30; then
    log "failed to get QMP socket for $node_id, aborting"
    bash "$CLEANUP_SCRIPT" >/dev/null 2>&1 || true
    exit 1
  fi
  cont_qemu "$qmp_socket"
  log "resumed ${node_id}"
done

cat > "$ENV_FILE" <<EOF
export RUN_ID='$RUN_ID'
export RUN_DIR='$(dirname "$CONTROL_LOG")'
export CLEANUP_SCRIPT='$CLEANUP_SCRIPT'
export SIM_UAPI_W5_PROFILE='$SIM_UAPI_W5_PROFILE'
export NODEA_SERIAL_SOCKET='$SERIAL_DIR/nodeA.${SOCKET_SUFFIX}.sock'
export NODEB_SERIAL_SOCKET='$SERIAL_DIR/nodeB.${SOCKET_SUFFIX}.sock'
export NODEC_SERIAL_SOCKET='$SERIAL_DIR/nodeC.${SOCKET_SUFFIX}.sock'
export NODED_SERIAL_SOCKET='$SERIAL_DIR/nodeD.${SOCKET_SUFFIX}.sock'
export NODEE_SERIAL_SOCKET='$SERIAL_DIR/nodeE.${SOCKET_SUFFIX}.sock'
export NODEF_SERIAL_SOCKET='$SERIAL_DIR/nodeF.${SOCKET_SUFFIX}.sock'
export NODEG_SERIAL_SOCKET='$SERIAL_DIR/nodeG.${SOCKET_SUFFIX}.sock'
export NODEH_SERIAL_SOCKET='$SERIAL_DIR/nodeH.${SOCKET_SUFFIX}.sock'
EOF

log "env_file=$ENV_FILE"
log "cleanup=$CLEANUP_SCRIPT"
echo "$ENV_FILE"
