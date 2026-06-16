#!/bin/zsh
set -euo pipefail

# OBMM GSVA ARM MMU Acceptance - Two Node
# Runs OBMM GSVA identity test with ARM MMU mode enabled.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-${GSVA_DEMO_MODE:-identity}}"
export OBMM_GSVA_BASE="${OBMM_GSVA_BASE:-${GSVA_DEMO_BASE:-0x700000000000}}"
export OBMM_GSVA_SIZE="${OBMM_GSVA_SIZE:-${GSVA_DEMO_SIZE:-0x400000}}"
export OBMM_GSVA_NODE_COUNT="${OBMM_GSVA_NODE_COUNT:-${GSVA_DEMO_NODE_COUNT:-2}}"
export GSVA_MODE="${GSVA_MODE:-arm_mmu}"
export GSVA_STRICT="${GSVA_STRICT:-1}"
export APPEND_EXTRA="${APPEND_EXTRA:-gsva_mode=arm_mmu gsva_strict=1}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_armmmu_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_dual_node_obmm_gsva.sh" "$@"
