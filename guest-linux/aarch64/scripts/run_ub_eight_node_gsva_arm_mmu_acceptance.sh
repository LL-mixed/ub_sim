#!/bin/zsh
set -euo pipefail

# OBMM GSVA ARM MMU Acceptance - Eight Node

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export OBMM_GSVA_MODE="${OBMM_GSVA_MODE:-${GSVA_DEMO_MODE:-matrix}}"
export OBMM_GSVA_MATRIX_BASE="${OBMM_GSVA_MATRIX_BASE:-${GSVA_DEMO_BASE:-0x700000000000}}"
export OBMM_GSVA_MATRIX_SLICE_SIZE="${OBMM_GSVA_MATRIX_SLICE_SIZE:-${GSVA_DEMO_SIZE:-0x400000}}"
export OBMM_GSVA_MATRIX_NODE_COUNT="${OBMM_GSVA_MATRIX_NODE_COUNT:-${GSVA_DEMO_NODE_COUNT:-${GSVA_MATRIX_NODE_COUNT:-8}}}"
export GSVA_MODE="${GSVA_MODE:-arm_mmu}"
export GSVA_STRICT="${GSVA_STRICT:-1}"
export APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% obmm.mempool_size=0 gsva_mode=arm_mmu gsva_strict=1}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_armmmu8_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_multi_node_obmm_gsva_matrix.sh" "$@"
