#!/bin/zsh
set -euo pipefail

# ---------------------------------------------------------------------------
# launch_oe_four_node_tmux.sh
# Boot a 4-node openEuler simulated cluster and attach each node's serial
# console to its own tmux pane (titled nodeA-nodeD).
#
# Usage:
#   ./scripts/launch_oe_four_node_tmux.sh [--disk PATH] [--memory SIZE]
#        [--smp N] [--app-dir DIR] [--app-mode MODE]
#        [--session NAME] [--no-attach]
#
# Inside the session (prefix Ctrl-b):
#   :setw synchronize-panes    broadcast keystrokes to all four nodes
#   d                          detach (cluster keeps running)
# Cleanup:
#   bash <printed cleanup script>
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

DISK_IMAGE="/sd_data/vms/openEuler-2403/disk.qcow2"
MEMORY="4G"
SMP="4"
SESSION_NAME="oe4"
APP_DIR_ARG=()
ATTACH=1

usage() {
  cat >&2 <<'USAGE'
usage: launch_oe_four_node_tmux.sh [--disk PATH] [--memory SIZE] [--smp N]
       [--app-dir DIR] [--app-mode MODE] [--session NAME] [--no-attach] [-h]
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --disk)
      DISK_IMAGE="$2"; shift 2 ;;
    --memory)
      MEMORY="$2"; shift 2 ;;
    --smp)
      SMP="$2"; shift 2 ;;
    --app-dir)
      APP_DIR_ARG+=(--app-dir "$2"); shift 2 ;;
    --app-mode)
      APP_DIR_ARG+=(--app-mode "$2"); shift 2 ;;
    --session)
      SESSION_NAME="$2"; shift 2 ;;
    --no-attach)
      ATTACH=0; shift ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "missing required command: $1" >&2
    exit 1
  }
}
need_cmd tmux
need_cmd socat
[[ -f "$DISK_IMAGE" ]] || { echo "disk image not found: $DISK_IMAGE" >&2; exit 1; }
if tmux has-session -t "=$SESSION_NAME" 2>/dev/null; then
  echo "tmux session '$SESSION_NAME' already exists (try: tmux attach -t $SESSION_NAME)" >&2
  exit 1
fi

RUN_ID="oe4_$(date +%Y-%m-%d_%H-%M-%S)_$$"
SHARED_DIR="/tmp/ub-qemu-links-oe-${RUN_ID}"
SERIAL_DIR="$SHARED_DIR/serial"
CLEANUP_SCRIPT="$ROOT_DIR/out/openEuler-super-node/nodes/${RUN_ID}/cleanup.sh"

echo "[oe4-tmux] launching 4-node openEuler cluster (run_id=$RUN_ID)..."
UB_FM_SHARED_DIR="$SHARED_DIR" \
  "$SCRIPT_DIR/run-openEuler-simulated-super-node.sh" \
    --disk "$DISK_IMAGE" --nodes 4 --memory "$MEMORY" --smp "$SMP" \
    --run-id "$RUN_ID" "${APP_DIR_ARG[@]}"

# Wait until all four serial sockets accept connections.
NODE_IDS=(nodeA nodeB nodeC nodeD)
wait_attempts=0
until [[ -S "$SERIAL_DIR/nodeD.sock" && -S "$SERIAL_DIR/nodeA.sock" &&
         -S "$SERIAL_DIR/nodeB.sock" && -S "$SERIAL_DIR/nodeC.sock" ]]; do
  (( wait_attempts++ < 100 )) || { echo "serial sockets never appeared in $SERIAL_DIR" >&2; exit 1; }
  sleep 0.5
done

echo "[oe4-tmux] creating tmux session $SESSION_NAME with 4 console panes..."
tmux new-session -d -s "$SESSION_NAME" -n nodes \
  "socat - UNIX-CONNECT:$SERIAL_DIR/nodeA.sock"
tmux split-window -t "$SESSION_NAME:0.0" \
  "socat - UNIX-CONNECT:$SERIAL_DIR/nodeB.sock"
tmux split-window -t "$SESSION_NAME:0.1" \
  "socat - UNIX-CONNECT:$SERIAL_DIR/nodeC.sock"
tmux split-window -t "$SESSION_NAME:0.0" \
  "socat - UNIX-CONNECT:$SERIAL_DIR/nodeD.sock"
tmux select-layout -t "$SESSION_NAME" tiled
tmux select-pane -t "$SESSION_NAME:0.0" -T nodeA
tmux select-pane -t "$SESSION_NAME:0.1" -T nodeB
tmux select-pane -t "$SESSION_NAME:0.2" -T nodeC
tmux select-pane -t "$SESSION_NAME:0.3" -T nodeD
tmux set-option -t "$SESSION_NAME" pane-border-status top
tmux set-option -t "$SESSION_NAME" pane-border-format ' #{pane_index} #{pane_title} '

cat <<EOF

[oe4-tmux] session '$SESSION_NAME' ready: 4 panes (nodeA-nodeD) on serial consoles.
[oe4-tmux] nodes reach 'localhost login:' ~60-90s after this line.
[oe4-tmux] broadcast keystrokes to all nodes: Ctrl-b : setw synchronize-panes
[oe4-tmux] detach: Ctrl-b d          re-attach: tmux attach -t $SESSION_NAME
[oe4-tmux] cleanup when done: bash $CLEANUP_SCRIPT

EOF

if (( ATTACH )) && [[ -t 1 ]]; then
  exec tmux attach -t "$SESSION_NAME"
fi
