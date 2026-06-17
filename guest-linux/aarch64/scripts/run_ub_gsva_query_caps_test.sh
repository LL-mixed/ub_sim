#!/bin/zsh
set -euo pipefail
setopt null_glob

# GSVA Query Caps Test - Single Node
#
# Runs gsva_query --caps on a 1-node QEMU setup to verify
# the GSVA V1 query path through OBMM ioctl -> kernel -> QEMU.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_qc_${RANDOM}}"
RUN_SECS="${RUN_SECS:-60}"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "linqu_probe_skip=1")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

mkdir -p "$LOG_DIR/${RUN_ID}"
GUEST_LOG="$LOG_DIR/${RUN_ID}/guest.log"
QEMU_LOG="$LOG_DIR/${RUN_ID}/qemu.log"
PID_FILE="$OUT_DIR/ub_node.gsva_query.${RUN_ID}.pid"

cleanup() {
  if [[ -f "$PID_FILE" ]]; then
    local pid
    pid="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
      sleep 0.5
      kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
  fi
}
trap 'cleanup' EXIT INT TERM

echo "[gsva_query] run_id=$RUN_ID"

env \
  UB_FM_NODE_ID="nodeA" \
  UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
  UB_FM_SHARED_DIR="/tmp/ub-qemu-links-gsva-query" \
  "$QEMU_BIN" \
    -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
    -cpu cortex-a57 \
    -m 2G \
    -nodefaults \
    -nographic \
    -serial file:"$GUEST_LOG" \
    -kernel "$KERNEL_IMAGE" \
    -initrd "$INITRAMFS_IMAGE" \
    -append "console=ttyAMA0 rdinit=/bin/run_demo linqu_gsva_query=1 gsva_query_mode=caps ${APPEND_EXTRA}" \
    >"$QEMU_LOG" 2>&1 &
echo $! > "$PID_FILE"

echo "[gsva_query] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  if [[ -f "$GUEST_LOG" ]] && grep -q 'verdict=PASS' "$GUEST_LOG"; then
    cleanup
    sleep 0.5
    echo "[gsva_query] PASS"
    grep 'gsva_query' "$GUEST_LOG" || true
    if grep -q 'GSVA_KEY' "$QEMU_LOG"; then
      echo "[gsva_query] GSVA_KEY found in QEMU log"
    fi
    exit 0
  fi
  if grep -qE 'verdict=FAIL|Kernel panic - not syncing|Call trace:' "$GUEST_LOG" 2>/dev/null; then
    echo "[gsva_query] FAIL: guest reported failure" >&2
    tail -20 "$GUEST_LOG" >&2
    exit 1
  fi
  sleep 0.5
done

echo "[gsva_query] FAIL: timeout" >&2
exit 1
