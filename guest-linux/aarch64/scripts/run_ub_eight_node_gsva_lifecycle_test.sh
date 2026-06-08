#!/bin/zsh
set -euo pipefail

# GSVA Lifecycle Test - Eight Node
# Wraps the 2-node lifecycle test script with 8-node topology.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

export TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini}"
export UB_FM_ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
export UB_SIM_ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
export UB_SIM_PORT_NUM="${UB_SIM_PORT_NUM:-7}"
export SHARED_DIR="${UB_FM_SHARED_DIR:-$WORKSPACE_ROOT/guest-linux/aarch64/out/gsva_lc8_links_${RANDOM}}"
export RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gsva_lc8_${RANDOM}}"

exec "$SCRIPT_DIR/run_ub_two_node_gsva_lifecycle_test.sh" "$@"
