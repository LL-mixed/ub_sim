#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../../.." && pwd)"

KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
RDINIT="${RDINIT:-/bin/run_demo}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/simulator/vendor/ub_topology_two_node_v0.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/simulator/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-dual}"
QMP_DIR="${SHARED_DIR}/qmp"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_tmux_${RANDOM}}"
SESSION_NAME="${TMUX_SESSION_NAME:-ub-dual-node-${RUN_ID}}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE="${TMUX_PORT_BASE:-$((45000 + RANDOM % 10000))}"
NODEA_MON_PORT="${NODEA_MON_PORT:-$((PORT_BASE + 0))}"
NODEB_MON_PORT="${NODEB_MON_PORT:-$((PORT_BASE + 1))}"
NODEA_SERIAL_PORT="${NODEA_SERIAL_PORT:-$((PORT_BASE + 2))}"
NODEB_SERIAL_PORT="${NODEB_SERIAL_PORT:-$((PORT_BASE + 3))}"
CONTROL_LOG="$LOG_DIR/${RUN_ID}_tmux/control.log"
CONTROL_SCRIPT="$OUT_DIR/tmux_dual_node_control.${RUN_ID}.sh"
CLEANUP_SCRIPT="$OUT_DIR/tmux_dual_node_cleanup.${RUN_ID}.sh"
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.tmux.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.tmux.${RUN_ID}.pid"
NODEA_QMP="$QMP_DIR/nodeA.${RUN_ID}.sock"
NODEB_QMP="$QMP_DIR/nodeB.${RUN_ID}.sock"
NODEA_QEMU_LOG="$LOG_DIR/${RUN_ID}_tmux/nodeA_qemu.log"
NODEB_QEMU_LOG="$LOG_DIR/${RUN_ID}_tmux/nodeB_qemu.log"
NODEA_GUEST_LOG="$LOG_DIR/${RUN_ID}_tmux/nodeA_guest.log"
NODEB_GUEST_LOG="$LOG_DIR/${RUN_ID}_tmux/nodeB_guest.log"

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
  cat <<EOF
echo "[tmux] waiting for ${label} on 127.0.0.1:${port}"
while ! nc -z 127.0.0.1 ${port} >/dev/null 2>&1; do sleep 0.2; done
echo "[tmux] connected to ${label}"
nc 127.0.0.1 ${port} || true
echo "[tmux] ${label} disconnected"
exec \${SHELL:-/bin/zsh} -i
EOF
}

need_cmd tmux
need_cmd nc
need_cmd python3

# Fail before creating tmux windows when required artifacts are not ready.
QEMU_BIN_PRECHECK="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$OUT_DIR" "$LOG_DIR/${RUN_ID}_tmux" "$QMP_DIR"

if tmux has-session -t "$SESSION_NAME" 2>/dev/null; then
  echo "tmux session already exists: $SESSION_NAME" >&2
  exit 1
fi

cat > "$CLEANUP_SCRIPT" <<EOF
#!/bin/zsh
set -euo pipefail
for pid_file in "$NODEA_PID_FILE" "$NODEB_PID_FILE"; do
  if [[ -f "\$pid_file" ]]; then
    pid="\$(cat "\$pid_file" 2>/dev/null || true)"
    if [[ -n "\${pid:-}" ]] && kill -0 "\$pid" 2>/dev/null; then
      kill "\$pid" 2>/dev/null || true
      sleep 0.2
      kill -9 "\$pid" 2>/dev/null || true
    fi
    rm -f "\$pid_file"
  fi
done
rm -f "$NODEA_QMP" "$NODEB_QMP"
echo "cleaned session run_id=$RUN_ID"
EOF
chmod +x "$CLEANUP_SCRIPT"

cat > "$CONTROL_SCRIPT" <<EOF
#!/bin/zsh
set -euo pipefail

source "$SCRIPT_DIR/qemu_ub_common.sh"

APPEND_EXTRA='$APPEND_EXTRA'
WORKSPACE_ROOT='$WORKSPACE_ROOT'
ROOT_DIR='$ROOT_DIR'
KERNEL_IMAGE='$KERNEL_IMAGE'
INITRAMFS_IMAGE='$INITRAMFS_IMAGE'
RDINIT='$RDINIT'
TOPOLOGY_FILE='$TOPOLOGY_FILE'
ENTITY_PLAN_FILE='$ENTITY_PLAN_FILE'
ENTITY_COUNT='$ENTITY_COUNT'
SHARED_DIR='$SHARED_DIR'
QMP_DIR='$QMP_DIR'
CONTROL_LOG='$CONTROL_LOG'
CLEANUP_SCRIPT='$CLEANUP_SCRIPT'
NODEA_PID_FILE='$NODEA_PID_FILE'
NODEB_PID_FILE='$NODEB_PID_FILE'
NODEA_QMP='$NODEA_QMP'
NODEB_QMP='$NODEB_QMP'
NODEA_QEMU_LOG='$NODEA_QEMU_LOG'
NODEB_QEMU_LOG='$NODEB_QEMU_LOG'
NODEA_GUEST_LOG='$NODEA_GUEST_LOG'
NODEB_GUEST_LOG='$NODEB_GUEST_LOG'
NODEA_MON_PORT='$NODEA_MON_PORT'
NODEB_MON_PORT='$NODEB_MON_PORT'
NODEA_SERIAL_PORT='$NODEA_SERIAL_PORT'
NODEB_SERIAL_PORT='$NODEB_SERIAL_PORT'
RUN_ID='$RUN_ID'
SESSION_NAME='$SESSION_NAME'

log() {
  echo "[tmux-control] \$*" | tee -a "\$CONTROL_LOG"
}

ipourma_ipv4_args_for_role() {
  local role="\$1"
  case "\$role" in
    nodeA)
      echo "linqu_ipourma_ipv4=10.0.0.1 linqu_ipourma_peer_ipv4=10.0.0.2"
      ;;
    nodeB)
      echo "linqu_ipourma_ipv4=10.0.0.2 linqu_ipourma_peer_ipv4=10.0.0.1"
      ;;
    *)
      ;;
  esac
}

cont_qemu() {
  local qmp_socket="\$1"
  python3 - <<PY
import socket
path = r"""\$qmp_socket"""
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(5)
s.connect(path)
s.recv(4096)
s.sendall(b'{"execute":"qmp_capabilities"}\\r\\n')
s.recv(4096)
s.sendall(b'{"execute":"cont"}\\r\\n')
s.recv(4096)
s.close()
PY
}

start_node() {
  local node_id="\$1"
  local role="\$2"
  local mon_port="\$3"
  local serial_port="\$4"
  local qemu_log="\$5"
  local guest_log="\$6"
  local pid_file="\$7"
  local qmp_socket="\$8"
  local node_append_extra="\$APPEND_EXTRA"
  local ipourma_args=""

  ipourma_args="\$(ipourma_ipv4_args_for_role "\$role")"
  if [[ -n "\$ipourma_args" ]]; then
    node_append_extra="\${node_append_extra} \${ipourma_args}"
  fi

  mkdir -p "\$(dirname "\$qemu_log")" "\$(dirname "\$guest_log")" "\$(dirname "\$qmp_socket")"

  env \\
    UB_FM_NODE_ID="\$node_id" \\
    UB_FM_TOPOLOGY_FILE="\$TOPOLOGY_FILE" \\
    UB_FM_SHARED_DIR="\$SHARED_DIR" \\
    UB_SIM_ENTITY_COUNT="\$ENTITY_COUNT" \\
    UB_FM_ENTITY_PLAN_FILE="\$ENTITY_PLAN_FILE" \\
    "\$QEMU_BIN" \\
      -S \\
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \\
      -cpu cortex-a57 \\
      -m 8G \\
      -nodefaults \\
      -display none \\
      -qmp unix:"\$qmp_socket",server=on,wait=off \\
      -chardev socket,id=mon0,host=127.0.0.1,port="\$mon_port",server=on,wait=off,telnet=off \\
      -mon chardev=mon0,mode=readline \\
      -chardev socket,id=ser0,host=127.0.0.1,port="\$serial_port",server=on,wait=off,telnet=off,logfile="\$guest_log",logappend=on \\
      -serial chardev:ser0 \\
      -kernel "\$KERNEL_IMAGE" \\
      -initrd "\$INITRAMFS_IMAGE" \\
      -append "console=ttyAMA0 rdinit=\${RDINIT} linqu_urma_dp_role=\${role} \${node_append_extra}" \\
      >"\$qemu_log" 2>&1 &
  echo \$! > "\$pid_file"
}

QEMU_BIN="\$(ensure_qemu_ub_binary "\$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "\$ROOT_DIR" "\$KERNEL_IMAGE" "\$INITRAMFS_IMAGE"

rm -f /tmp/ub-qemu/ub-bus-instance-*.lock
rm -f "\$NODEA_QMP" "\$NODEB_QMP"
touch "\$CONTROL_LOG"

log "session=\$SESSION_NAME run_id=\$RUN_ID"
log "qemu_bin=\$QEMU_BIN"
log "append_extra=\$APPEND_EXTRA"
log "logs_dir=$(dirname "$CONTROL_LOG")"

start_node nodeA nodeA "\$NODEA_MON_PORT" "\$NODEA_SERIAL_PORT" "\$NODEA_QEMU_LOG" "\$NODEA_GUEST_LOG" "\$NODEA_PID_FILE" "\$NODEA_QMP"
sleep 0.5
start_node nodeB nodeB "\$NODEB_MON_PORT" "\$NODEB_SERIAL_PORT" "\$NODEB_QEMU_LOG" "\$NODEB_GUEST_LOG" "\$NODEB_PID_FILE" "\$NODEB_QMP"

log "waiting for QMP sockets"
while [[ ! -S "\$NODEA_QMP" || ! -S "\$NODEB_QMP" ]]; do
  sleep 0.1
done

cont_qemu "\$NODEA_QMP"
cont_qemu "\$NODEB_QMP"

log "nodeA qemu_log=\$NODEA_QEMU_LOG"
log "nodeB qemu_log=\$NODEB_QEMU_LOG"
log "nodeA guest_log=\$NODEA_GUEST_LOG"
log "nodeB guest_log=\$NODEB_GUEST_LOG"
log "nodeA monitor tcp=127.0.0.1:\$NODEA_MON_PORT"
log "nodeB monitor tcp=127.0.0.1:\$NODEB_MON_PORT"
log "nodeA serial tcp=127.0.0.1:\$NODEA_SERIAL_PORT"
log "nodeB serial tcp=127.0.0.1:\$NODEB_SERIAL_PORT"
log "cleanup=\$CLEANUP_SCRIPT"
log "interactive shell ready"

export RUN_ID SESSION_NAME CONTROL_LOG CLEANUP_SCRIPT
export NODEA_QEMU_LOG NODEB_QEMU_LOG NODEA_GUEST_LOG NODEB_GUEST_LOG
export NODEA_MON_PORT NODEB_MON_PORT NODEA_SERIAL_PORT NODEB_SERIAL_PORT
exec \${SHELL:-/bin/zsh} -i
EOF
chmod +x "$CONTROL_SCRIPT"

tmux new-session -d -s "$SESSION_NAME" -n control "/bin/zsh -lc '$CONTROL_SCRIPT'"
tmux new-window -t "$SESSION_NAME":1 -n nodeA-qemu "/bin/zsh -lc '$(wait_port_cmd "nodeA qemu monitor" "$NODEA_MON_PORT")'"
tmux new-window -t "$SESSION_NAME":2 -n nodeB-qemu "/bin/zsh -lc '$(wait_port_cmd "nodeB qemu monitor" "$NODEB_MON_PORT")'"
tmux new-window -t "$SESSION_NAME":3 -n nodeA-guest "/bin/zsh -lc '$(wait_port_cmd "nodeA guest serial" "$NODEA_SERIAL_PORT")'"
tmux new-window -t "$SESSION_NAME":4 -n nodeB-guest "/bin/zsh -lc '$(wait_port_cmd "nodeB guest serial" "$NODEB_SERIAL_PORT")'"
tmux select-window -t "$SESSION_NAME":0

echo "tmux session: $SESSION_NAME"
echo "run_id: $RUN_ID"
echo "control log: $CONTROL_LOG"
echo "cleanup: $CLEANUP_SCRIPT"

if [[ -n "${TMUX:-}" ]]; then
  tmux switch-client -t "$SESSION_NAME"
else
  exec tmux attach -t "$SESSION_NAME"
fi
