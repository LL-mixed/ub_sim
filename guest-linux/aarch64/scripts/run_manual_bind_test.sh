#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_manual_bind_${RANDOM}}"
RUN_LOG_DIR="$LOG_DIR/${RUN_ID_BASE}_manual_bind_iter1"
RUN_LOG_FILE="$RUN_LOG_DIR/manual_bind_test.log"

echo "[TEST] Building initramfs with manual bind test..."
(
    cd "$ROOT_DIR"
    export INIT_TO_USE="$ROOT_DIR/out/init_manual_bind"
    "$SCRIPT_DIR/build_initramfs.sh"
)

echo "[TEST] Running QEMU with manual bind test..."
mkdir -p "$RUN_LOG_DIR"
RUN_ID="$RUN_ID_BASE" \
"$SCRIPT_DIR/run_ub_dual_node_urma_dataplane_workload_test.sh" \
  2>&1 | tee "$RUN_LOG_FILE"

ln -sfn "$RUN_LOG_FILE" "$BUILD_DIR/manual_bind_test.log"
