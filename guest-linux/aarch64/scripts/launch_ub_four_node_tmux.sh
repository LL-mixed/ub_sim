#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../../.." && pwd)"

KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_demo}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/simulator/vendor/ub_topology_four_node_full_mesh.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/simulator/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-four}"
QMP_DIR="${SHARED_DIR}/qmp"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_tmux4_${RANDOM}}"
SESSION_NAME="${TMUX_SESSION_NAME:-ub-four-node-${RUN_ID}}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE="${TMUX_PORT_BASE:-$((46000 + RANDOM % 10000))}"
CREATE_QEMU_WINDOWS="${TMUX_QEMU_WINDOWS:-1}"
CREATE_GUEST_WINDOWS="${TMUX_GUEST_WINDOWS:-1}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
CONTROL_LOG="$LOG_DIR/${RUN_ID}_tmux4/control.log"
CONTROL_SCRIPT="$OUT_DIR/tmux_four_node_control.${RUN_ID}.sh"
CLEANUP_SCRIPT="$OUT_DIR/tmux_four_node_cleanup.${RUN_ID}.sh"

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

wait_port_cmd() {
  local label="$1"
  local port="$2"
  cat <<EOC

echo "[tmux] waiting for ${label} on 127.0.0.1:${port}"
while ! nc -z 127.0.0.1 ${port} >/dev/null 2>&1; do sleep 0.2; done
while true; do
  echo "[tmux] connected to ${label}"
  nc 127.0.0.1 ${port} || true
  echo "[tmux] ${label} disconnected"
  sleep 1
  while ! nc -z 127.0.0.1 ${port} >/dev/null 2>&1; do sleep 0.5; done
done
EOC
}

need_cmd tmux
need_cmd nc
need_cmd python3

QEMU_BIN_PRECHECK="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$OUT_DIR" "$LOG_DIR/${RUN_ID}_tmux4" "$QMP_DIR"

if [[ ! -f "$TOPOLOGY_FILE" ]]; then
  echo "TOPOLOGY_FILE not found: $TOPOLOGY_FILE" >&2
  exit 1
fi

if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "tmux session already exists: $SESSION_NAME" >&2
  exit 1
fi

cat > "$CLEANUP_SCRIPT" <<'EOC'
#!/bin/zsh
set -euo pipefail
NODE_IDS=(nodeA nodeB nodeC nodeD)
for node_id in "${NODE_IDS[@]}"; do
  pid_file="__OUT_DIR__/ub_${node_id}.tmux.__RUN_ID__.pid"
  if [[ -f "$pid_file" ]]; then
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      kill "$pid" 2>/dev/null || true
      sleep 0.2
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
  fi
  rm -f "__QMP_DIR__/${node_id}.__RUN_ID__.sock"
done
echo "cleaned session run_id=__RUN_ID__"
EOC
perl -0pi -e 's#__OUT_DIR__#'$OUT_DIR'#g; s#__RUN_ID__#'$RUN_ID'#g; s#__QMP_DIR__#'$QMP_DIR'#g' "$CLEANUP_SCRIPT"
chmod +x "$CLEANUP_SCRIPT"

cat > "$CONTROL_SCRIPT" <<'EOC'
#!/bin/zsh
set -euo pipefail

source "__SCRIPT_DIR__/qemu_ub_common.sh"

APPEND_EXTRA='__APPEND_EXTRA__'
WORKSPACE_ROOT='__WORKSPACE_ROOT__'
ROOT_DIR='__ROOT_DIR__'
KERNEL_IMAGE='__KERNEL_IMAGE__'
INITRAMFS_IMAGE='__INITRAMFS_IMAGE__'
RDINIT='__RDINIT__'
TOPOLOGY_FILE='__TOPOLOGY_FILE__'
ENTITY_PLAN_FILE='__ENTITY_PLAN_FILE__'
ENTITY_COUNT='__ENTITY_COUNT__'
SHARED_DIR='__SHARED_DIR__'
QMP_DIR='__QMP_DIR__'
CONTROL_LOG='__CONTROL_LOG__'
CLEANUP_SCRIPT='__CLEANUP_SCRIPT__'
RUN_ID='__RUN_ID__'
SESSION_NAME='__SESSION_NAME__'
NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)
PORT_BASE='__PORT_BASE__'
BOOT_WAIT_SECS='__BOOT_WAIT_SECS__'

log() {
  echo "[tmux4-control] $*" | tee -a "$CONTROL_LOG"
}

wait_guest_shell_gate() {
  local guest_log="$1"
  local node_id="$2"
  local timeout_s="$3"

  python3 - "$guest_log" "$node_id" "$timeout_s" <<'PY'
import pathlib
import sys
import time

guest_log = pathlib.Path(sys.argv[1])
node_id = sys.argv[2]
timeout_s = int(sys.argv[3])
deadline = time.time() + timeout_s
ok_marker = "[run_demo] boot flow completed, dropping to shell"
bad_markers = ("Kernel panic", "No working init found", "TIMEOUT")

while time.time() < deadline:
    text = guest_log.read_text(errors="ignore") if guest_log.exists() else ""
    if ok_marker in text:
        print(f"[tmux4-control] {node_id} bootstrap shell gate ok")
        sys.exit(0)
    for marker in bad_markers:
        if marker in text:
            print(f"[tmux4-control] {node_id} bootstrap failed marker={marker}")
            sys.exit(2)
    time.sleep(1)

print(f"[tmux4-control] {node_id} bootstrap shell gate timeout after {timeout_s}s")
sys.exit(1)
PY
}

cont_qemu() {
  local qmp_socket="$1"
  local attempt=0
  while (( attempt < 50 )); do
    if python3 - <<PY
import socket
path = r"""$qmp_socket"""
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

  mkdir -p "$(dirname "$qemu_log")" "$(dirname "$guest_log")" "$(dirname "$qmp_socket")"

  env \
    UB_FM_NODE_ID="$node_id" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    "$QEMU_BIN" \
      -S \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -m 8G \
      -nodefaults \
      -display none \
      -qmp unix:"$qmp_socket",server=on,wait=off \
      -chardev socket,id=mon0,host=127.0.0.1,port="$mon_port",server=on,wait=off,telnet=off \
      -mon chardev=mon0,mode=readline \
      -chardev socket,id=ser0,host=127.0.0.1,port="$serial_port",server=on,wait=off,telnet=off,logfile="$guest_log",logappend=on \
      -serial chardev:ser0 \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=${RDINIT} ${node_append_extra}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

rm -f /tmp/ub-qemu/ub-bus-instance-*.lock(N)
rm -f "$QMP_DIR"/*.sock(N)
touch "$CONTROL_LOG"

log "session=$SESSION_NAME run_id=$RUN_ID"
log "qemu_bin=$QEMU_BIN"
log "topology=$TOPOLOGY_FILE"
log "append_extra=$APPEND_EXTRA"
log "logs_dir=$(dirname "$CONTROL_LOG")"
log "boot_wait_secs=$BOOT_WAIT_SECS"

integer idx=0
for node_id in "${NODE_IDS[@]}"; do
  local_ip="${NODE_IPS[$((idx+1))]}"
  mon_port=$((PORT_BASE + idx))
  serial_port=$((PORT_BASE + 16 + idx))
  qemu_log="$(dirname "$CONTROL_LOG")/${node_id}_qemu.log"
  guest_log="$(dirname "$CONTROL_LOG")/${node_id}_guest.log"
  pid_file="$ROOT_DIR/out/ub_${node_id}.tmux.${RUN_ID}.pid"
  qmp_socket="$QMP_DIR/${node_id}.${RUN_ID}.sock"

  log "starting ${node_id} (paused) local_ip=${local_ip} mon=${mon_port} serial=${serial_port}"
  start_node "$node_id" "$local_ip" "$mon_port" "$serial_port" "$qemu_log" "$guest_log" "$pid_file" "$qmp_socket"
  idx=$((idx + 1))
  sleep 0.3
done

log "waiting for QMP sockets"
for node_id in "${NODE_IDS[@]}"; do
  qmp_socket="$QMP_DIR/${node_id}.${RUN_ID}.sock"
  while [[ ! -S "$qmp_socket" ]]; do
    sleep 0.1
  done
  cont_qemu "$qmp_socket"
  log "resumed ${node_id}"
done

idx=0
for node_id in "${NODE_IDS[@]}"; do
  mon_port=$((PORT_BASE + idx))
  serial_port=$((PORT_BASE + 16 + idx))
  qemu_log="$(dirname "$CONTROL_LOG")/${node_id}_qemu.log"
  guest_log="$(dirname "$CONTROL_LOG")/${node_id}_guest.log"
  log "${node_id} qemu_log=${qemu_log}"
  log "${node_id} guest_log=${guest_log}"
  log "${node_id} monitor tcp=127.0.0.1:${mon_port}"
  log "${node_id} serial tcp=127.0.0.1:${serial_port}"
  idx=$((idx + 1))
done

for node_id in "${NODE_IDS[@]}"; do
  guest_log="$(dirname "$CONTROL_LOG")/${node_id}_guest.log"
  log "waiting bootstrap shell gate for ${node_id}"
  wait_guest_shell_gate "$guest_log" "$node_id" "$BOOT_WAIT_SECS" | tee -a "$CONTROL_LOG"
done

log "cleanup=$CLEANUP_SCRIPT"
log "interactive shells ready after /bin/run_demo bootstrap"

exec ${SHELL:-/bin/zsh} -i
EOC
perl -0pi -e 's#__SCRIPT_DIR__#'$SCRIPT_DIR'#g; s#__APPEND_EXTRA__#'"${APPEND_EXTRA//\//\\/}"'#g; s#__WORKSPACE_ROOT__#'$WORKSPACE_ROOT'#g; s#__ROOT_DIR__#'$ROOT_DIR'#g; s#__KERNEL_IMAGE__#'"${KERNEL_IMAGE//\//\\/}"'#g; s#__INITRAMFS_IMAGE__#'"${INITRAMFS_IMAGE//\//\\/}"'#g; s#__RDINIT__#'"${RDINIT//\//\\/}"'#g; s#__TOPOLOGY_FILE__#'"${TOPOLOGY_FILE//\//\\/}"'#g; s#__ENTITY_PLAN_FILE__#'"${ENTITY_PLAN_FILE//\//\\/}"'#g; s#__ENTITY_COUNT__#'$ENTITY_COUNT'#g; s#__SHARED_DIR__#'"${SHARED_DIR//\//\\/}"'#g; s#__QMP_DIR__#'"${QMP_DIR//\//\\/}"'#g; s#__CONTROL_LOG__#'"${CONTROL_LOG//\//\\/}"'#g; s#__CLEANUP_SCRIPT__#'"${CLEANUP_SCRIPT//\//\\/}"'#g; s#__RUN_ID__#'$RUN_ID'#g; s#__SESSION_NAME__#'$SESSION_NAME'#g; s#__PORT_BASE__#'$PORT_BASE'#g; s#__BOOT_WAIT_SECS__#'$BOOT_WAIT_SECS'#g' "$CONTROL_SCRIPT"
chmod +x "$CONTROL_SCRIPT"

tmux new-session -d -s "$SESSION_NAME" -n control "/bin/zsh -lc '$CONTROL_SCRIPT'"

integer idx=0
for node_id in "${NODE_IDS[@]}"; do
  mon_port=$((PORT_BASE + idx))
  serial_port=$((PORT_BASE + 16 + idx))
  if [[ "$CREATE_QEMU_WINDOWS" == "1" ]]; then
    tmux new-window -t "$SESSION_NAME" -n "${node_id}-qemu" "/bin/zsh -lc '$(wait_port_cmd "${node_id} qemu monitor" "$mon_port")'"
  fi
  if [[ "$CREATE_GUEST_WINDOWS" == "1" ]]; then
    tmux new-window -t "$SESSION_NAME" -n "${node_id}-guest" "/bin/zsh -lc '$(wait_port_cmd "${node_id} guest serial" "$serial_port")'"
  fi
  idx=$((idx + 1))
done

tmux select-window -t "$SESSION_NAME":0

echo "tmux session: $SESSION_NAME"
echo "run_id: $RUN_ID"
echo "control log: $CONTROL_LOG"
echo "cleanup: $CLEANUP_SCRIPT"

if [[ "${TMUX_ATTACH:-1}" == "0" ]]; then
  exit 0
fi

if [[ -n "${TMUX:-}" ]]; then
  tmux switch-client -t "$SESSION_NAME"
else
  exec tmux attach -t "$SESSION_NAME"
fi
