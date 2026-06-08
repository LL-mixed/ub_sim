#!/bin/zsh
set -euo pipefail

# GSVA Lifecycle Test - Four Node
# Wraps the 2-node lifecycle test script with 4-node topology.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh_one_entity.ini}"
export UB_FM_ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_one_entity.ini}"
export UB_SIM_ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-1}"
export UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-3}"
export SHARED_DIR="${UB_FM_SHARED_DIR:-$WORKSPACE_ROOT/guest-linux/aarch64/out/gsva_lc4_links_${RANDOM}}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_lc4_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_two_node_gsva_lifecycle_test.sh" "$@"
