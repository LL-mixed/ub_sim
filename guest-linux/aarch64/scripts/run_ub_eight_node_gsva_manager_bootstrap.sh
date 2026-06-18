#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export GVA_MANAGER_NODE_COUNT="${GVA_MANAGER_NODE_COUNT:-8}"
export TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini}"
export UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-7}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_mgr8_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_four_node_gsva_manager_bootstrap.sh" "$@"
