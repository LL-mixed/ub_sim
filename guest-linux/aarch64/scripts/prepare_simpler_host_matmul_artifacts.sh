#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${1:-${SIMPLER_HOST_MATMUL_ARTIFACT_DIR:-/tmp/simpler-host-matmul-artifacts}}"
shift $(( $# > 0 ? 1 : 0 ))

exec python3 "$SCRIPT_DIR/prepare_simpler_host_matmul_artifacts.py" --output-dir "$OUT_DIR" "$@"
