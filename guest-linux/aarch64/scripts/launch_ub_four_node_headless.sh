#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_app}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-four}"
QMP_DIR="${SHARED_DIR}/qmp"
SIMPLER_HOST_VECTOR_MANIFEST="${SIMPLER_HOST_VECTOR_MANIFEST:-/tmp/simpler-host-vector-artifacts/host_vector_manifest.json}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-host_vector}"
SIM_UAPI_SCENARIO_CONFIG="${SIM_UAPI_SCENARIO_CONFIG:-$WORKSPACE_ROOT/scenarios/mvp_4host_single_domain.yaml}"
REMOTE_MEMORY_MODEL_MANIFEST="${REMOTE_MEMORY_MODEL_MANIFEST:-}"
SCHEDULER_CORE_MODEL="${SCHEDULER_CORE_MODEL:-}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_headless4_${RANDOM}}"
# macOS UNIX domain socket path limit is 104 bytes. Use a short suffix for socket file names.
SOCKET_SUFFIX="${SOCKET_SUFFIX:-$$_${RANDOM}}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE="${PORT_BASE:-$((46000 + RANDOM % 10000))}"
QEMU_MEM="${QEMU_MEM:-2G}"
QEMU_SMP="${QEMU_SMP:-2}"
CONTROL_LOG="$LOG_DIR/${RUN_ID}_headless4/control.log"
CLEANUP_SCRIPT="$OUT_DIR/headless_four_node_cleanup.${RUN_ID}.sh"
ENV_FILE="$OUT_DIR/headless_four_node_env.${RUN_ID}.sh"

NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)

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
  echo "[headless4] $*" | tee -a "$CONTROL_LOG"
}

cont_qemu() {
  local qmp_socket="$1"
  local attempt=0
  while (( attempt < 50 )); do
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
  local mon_port="$3"
  local serial_port="$4"
  local qemu_log="$5"
  local guest_log="$6"
  local pid_file="$7"
  local qmp_socket="$8"
  local node_append_extra="$APPEND_EXTRA linqu_ipourma_ipv4=$local_ip"
  local remote_model_args=()
  local scheduler_core_args=()

  if [[ -n "$REMOTE_MEMORY_MODEL_MANIFEST" ]]; then
    remote_model_args=(
      -global "ubc.remote-memory-model-manifest=$REMOTE_MEMORY_MODEL_MANIFEST"
    )
  fi
  if [[ -n "$SCHEDULER_CORE_MODEL" ]]; then
    scheduler_core_args=(
      -global "ubc.scheduler-core-model=$SCHEDULER_CORE_MODEL"
    )
  fi

  (
    env \
      UB_FM_NODE_ID="$node_id" \
      UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
      UB_FM_SHARED_DIR="$SHARED_DIR" \
      UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
      UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
      SIMPLER_HOST_VECTOR_MANIFEST="$SIMPLER_HOST_VECTOR_MANIFEST" \
      SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
      SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
      SIM_QWEN3_DENSE_WEIGHTS_PATH="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" \
      SIM_UAPI_SCENARIO_CONFIG="$SIM_UAPI_SCENARIO_CONFIG" \
      "$QEMU_BIN" \
        -S \
        -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
        -cpu cortex-a57 \
        -smp "$QEMU_SMP" \
        -m "$QEMU_MEM" \
        -nodefaults \
        -display none \
        "${remote_model_args[@]}" \
        "${scheduler_core_args[@]}" \
        -qmp unix:"$qmp_socket",server=on,wait=off \
        -chardev socket,id=mon0,host=127.0.0.1,port="$mon_port",server=on,wait=off,telnet=off \
        -mon chardev=mon0,mode=readline \
        -chardev socket,id=ser0,host=127.0.0.1,port="$serial_port",server=on,wait=off,telnet=off,logfile="$guest_log",logappend=off \
        -serial chardev:ser0 \
        -kernel "$KERNEL_IMAGE" \
        -initrd "$INITRAMFS_IMAGE" \
        -append "console=ttyAMA0 rdinit=${RDINIT} ${node_append_extra}" \
        >"$qemu_log" 2>&1 &
    qemu_pid=$!
    echo "$qemu_pid" > "$pid_file"
    set +e
    wait "$qemu_pid"
    rc=$?
    set -e
    echo "[headless4] qemu_exit node=${node_id} pid=${qemu_pid} rc=${rc}" >> "$CONTROL_LOG"
  ) >>"$CONTROL_LOG" 2>&1 &!
  while [[ ! -f "$pid_file" ]]; do
    sleep 0.01
  done
}

need_cmd python3

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$OUT_DIR" "$LOG_DIR/${RUN_ID}_headless4" "$QMP_DIR"

if [[ ! -f "$TOPOLOGY_FILE" ]]; then
  echo "TOPOLOGY_FILE not found: $TOPOLOGY_FILE" >&2
  exit 1
fi

cat > "$CLEANUP_SCRIPT" <<'EOC'
#!/bin/zsh
set -euo pipefail
NODE_IDS=(nodeA nodeB nodeC nodeD)
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
done
rm -f /tmp/ub-qemu/ub-bus-instance-*.lock(N)
echo "cleaned run_id=__RUN_ID__"
EOC
perl -0pi -e 's#__OUT_DIR__#'"$OUT_DIR"'#g; s#__RUN_ID__#'"$RUN_ID"'#g; s#__SOCKET_SUFFIX__#'"$SOCKET_SUFFIX"'#g; s#__QMP_DIR__#'"$QMP_DIR"'#g' "$CLEANUP_SCRIPT"
chmod +x "$CLEANUP_SCRIPT"

cat > "$ENV_FILE" <<EOF
export RUN_ID='$RUN_ID'
export RUN_DIR='$(dirname "$CONTROL_LOG")'
export CLEANUP_SCRIPT='$CLEANUP_SCRIPT'
export QEMU_BIN='$QEMU_BIN'
export KERNEL_IMAGE='$KERNEL_IMAGE'
export INITRAMFS_IMAGE='$INITRAMFS_IMAGE'
export PORT_BASE='$PORT_BASE'
export NODEA_MON_PORT='$((PORT_BASE + 0))'
export NODEB_MON_PORT='$((PORT_BASE + 1))'
export NODEC_MON_PORT='$((PORT_BASE + 2))'
export NODED_MON_PORT='$((PORT_BASE + 3))'
export NODEA_SERIAL_PORT='$((PORT_BASE + 16))'
export NODEB_SERIAL_PORT='$((PORT_BASE + 17))'
export NODEC_SERIAL_PORT='$((PORT_BASE + 18))'
export NODED_SERIAL_PORT='$((PORT_BASE + 19))'
EOF

rm -f /tmp/ub-qemu/ub-bus-instance-*.lock(N)
rm -f "$QMP_DIR"/*.sock(N)
touch "$CONTROL_LOG"

log "run_id=$RUN_ID"
log "qemu_bin=$QEMU_BIN"
log "qemu_mem=$QEMU_MEM"
log "qemu_smp=$QEMU_SMP"
log "topology=$TOPOLOGY_FILE"
log "append_extra=$APPEND_EXTRA"
log "remote_memory_model_manifest=${REMOTE_MEMORY_MODEL_MANIFEST:-disabled}"
log "scheduler_core_model=${SCHEDULER_CORE_MODEL:-disabled}"
log "logs_dir=$(dirname "$CONTROL_LOG")"

integer idx=0
for node_id in "${NODE_IDS[@]}"; do
  local_ip="${NODE_IPS[$((idx+1))]}"
  mon_port=$((PORT_BASE + idx))
  serial_port=$((PORT_BASE + 16 + idx))
  qemu_log="$(dirname "$CONTROL_LOG")/${node_id}_qemu.log"
  guest_log="$(dirname "$CONTROL_LOG")/${node_id}_guest.log"
  pid_file="$OUT_DIR/ub_${node_id}.headless.${RUN_ID}.pid"
  qmp_socket="$QMP_DIR/${node_id}.${SOCKET_SUFFIX}.sock"

  log "starting ${node_id} local_ip=${local_ip} mon=${mon_port} serial=${serial_port}"
  start_node "$node_id" "$local_ip" "$mon_port" "$serial_port" "$qemu_log" "$guest_log" "$pid_file" "$qmp_socket"
  idx=$((idx + 1))
  sleep 0.3
done

log "waiting for QMP sockets"
for node_id in "${NODE_IDS[@]}"; do
  qmp_socket="$QMP_DIR/${node_id}.${SOCKET_SUFFIX}.sock"
  while [[ ! -S "$qmp_socket" ]]; do
    sleep 0.1
  done
  cont_qemu "$qmp_socket"
  log "resumed ${node_id}"
done

log "env_file=$ENV_FILE"
log "cleanup=$CLEANUP_SCRIPT"
echo "$ENV_FILE"
