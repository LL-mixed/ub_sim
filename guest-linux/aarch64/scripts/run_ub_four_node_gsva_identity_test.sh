#!/bin/zsh
set -euo pipefail

# GSVA Identity Test - Four Node
# Runs the GSVA matrix demo in identity mode as a formal acceptance test.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export GSVA_DEMO_MODE="${GSVA_DEMO_MODE:-matrix}"
export GSVA_DEMO_BASE="${GSVA_DEMO_BASE:-0x700000000000}"
export GSVA_DEMO_SIZE="${GSVA_DEMO_SIZE:-0x400000}"
export GSVA_DEMO_NODE_COUNT="${GSVA_DEMO_NODE_COUNT:-4}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_id4_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_four_node_gsva_matrix_demo.sh" "$@"
