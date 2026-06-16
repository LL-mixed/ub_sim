#!/bin/zsh
set -euo pipefail

# OBMM GSVA Identity Test - Two Node
# Runs the OBMM GSVA app in identity mode as a formal acceptance test.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-${GSVA_DEMO_MODE:-identity}}"
export OBMM_GSVA_BASE="${OBMM_GSVA_BASE:-${GSVA_DEMO_BASE:-0x700000000000}}"
export OBMM_GSVA_SIZE="${OBMM_GSVA_SIZE:-${GSVA_DEMO_SIZE:-0x400000}}"
export OBMM_GSVA_NODE_COUNT="${OBMM_GSVA_NODE_COUNT:-${GSVA_DEMO_NODE_COUNT:-2}}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_id_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_dual_node_obmm_gsva.sh" "$@"
