#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_w4_demo=1}"
export APPEND_EXTRA

exec "$SCRIPT_DIR/run_ub_dual_node_apps.sh"
