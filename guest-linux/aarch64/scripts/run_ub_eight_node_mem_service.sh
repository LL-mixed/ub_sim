#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# mem_service --smoke is a local metadata/CLI check and does not depend on
# topology width; keep the wider matrix entry lightweight and deterministic.
exec "$SCRIPT_DIR/run_ub_dual_node_mem_service.sh" "$@"
