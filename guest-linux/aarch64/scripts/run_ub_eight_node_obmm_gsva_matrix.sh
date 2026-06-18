#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

export OBMM_GSVA_MATRIX_NODE_COUNT=8
exec "$SCRIPT_DIR/run_ub_multi_node_obmm_gsva_matrix.sh" "$@"
