#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export COH_NODE_COUNT="${COH_NODE_COUNT:-8}"
export TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini}"
export UB_FM_ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
export UB_SIM_ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
export UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-7}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_coh8_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_four_node_obmm_coh_test.sh" "$@"
