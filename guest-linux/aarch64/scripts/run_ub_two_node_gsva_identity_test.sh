#!/bin/zsh
set -euo pipefail

# GSVA Identity Test - Two Node
# Runs the existing GSVA demo in identity mode as a formal acceptance test.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export GSVA_DEMO_MODE="${GSVA_DEMO_MODE:-identity}"
export GSVA_DEMO_BASE="${GSVA_DEMO_BASE:-0x700000000000}"
export GSVA_DEMO_SIZE="${GSVA_DEMO_SIZE:-0x400000}"
export GSVA_DEMO_NODE_COUNT="${GSVA_DEMO_NODE_COUNT:-2}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_id_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_dual_node_gsva_demo.sh" "$@"
