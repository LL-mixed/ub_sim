#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 pmd_mapping=100% w4_db_region_size_mb=512 obmm.mempool_size=512M}"
RUN_SECS="${RUN_SECS:-300}"
MAX_RUNTIME="${MAX_RUNTIME:-420}"
export APPEND_EXTRA
export RUN_SECS
export MAX_RUNTIME

exec "$SCRIPT_DIR/run_ub_dual_node_apps.sh" --app w4_guest "$@"
