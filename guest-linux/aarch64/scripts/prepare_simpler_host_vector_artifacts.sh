#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT_DIR="${1:-${SIMPLER_HOST_VECTOR_ARTIFACT_DIR:-/tmp/simpler-host-vector-artifacts}}"
shift $(( $# > 0 ? 1 : 0 ))

exec python3 "$SCRIPT_DIR/prepare_simpler_host_vector_artifacts.py" --output-dir "$OUT_DIR" "$@"
