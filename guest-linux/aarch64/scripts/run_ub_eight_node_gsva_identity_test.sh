#!/bin/zsh
set -euo pipefail

# OBMM GSVA Matrix Test - Eight Node
# Runs the OBMM GSVA app in matrix mode across 8 nodes as acceptance test.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-matrix}"
export OBMM_GSVA_MATRIX_BASE="${OBMM_GSVA_MATRIX_BASE:-0x700000000000}"
export OBMM_GSVA_MATRIX_SLICE_SIZE="${OBMM_GSVA_MATRIX_SLICE_SIZE:-0x400000}"
export OBMM_GSVA_MATRIX_NODE_COUNT="${OBMM_GSVA_MATRIX_NODE_COUNT:-${GSVA_MATRIX_NODE_COUNT:-8}}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_id8_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_multi_node_obmm_gsva_matrix.sh" "$@"
