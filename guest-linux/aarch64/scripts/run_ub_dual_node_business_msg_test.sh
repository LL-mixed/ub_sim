#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ITERATIONS="${ITERATIONS:-1}"
RUN_SECS="${RUN_SECS:-80}"
START_GAP_SECS="${START_GAP_SECS:-1}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_bizmsg_${RANDOM}}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_bizmsg_verify=1 linqu_urma_dp_verify=1}"

APPEND_EXTRA="$APPEND_EXTRA" \
RUN_SECS="$RUN_SECS" \
ITERATIONS="$ITERATIONS" \
START_GAP_SECS="$START_GAP_SECS" \
RUN_ID="$RUN_ID_BASE" \
"$SCRIPT_DIR/run_ub_dual_node_urma_dataplane_workload_test.sh"
