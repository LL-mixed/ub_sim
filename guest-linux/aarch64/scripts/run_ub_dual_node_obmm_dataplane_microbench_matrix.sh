#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE_RUNNER="$SCRIPT_DIR/run_ub_dual_node_obmm_dataplane_microbench.sh"
MODES=(legacy-pa generic-gva gsva)
RUNNER_ARGS=()

usage() {
  cat <<'EOF'
Usage: run_ub_dual_node_obmm_dataplane_microbench_matrix.sh [options]

Runs the 2-node OBMM dataplane microbench app across legacy PA, generic GVA,
and GSVA modes using the reusable app runner.

Options:
  --iters N            Benchmark iterations passed to each mode.
  --chunk-size BYTES   Per-iteration chunk size passed to each mode.
  --run-secs N         Completion timeout passed to each mode.
  --link-wait-secs N   FM link wait timeout passed to each mode.
  --verify             Enable data verification in each mode.
  -h, --help           Show this help.
EOF
}

while (( $# > 0 )); do
  case "$1" in
    --iters|--chunk-size|--run-secs|--link-wait-secs)
      if (( $# < 2 )); then
        echo "$1 requires a value" >&2
        exit 2
      fi
      RUNNER_ARGS+=("$1" "$2")
      shift 2
      ;;
    --verify)
      RUNNER_ARGS+=("$1")
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

for mode in "${MODES[@]}"; do
  printf '[dp_microbench_matrix] RUN mode=%s\n' "$mode"
  "$BASE_RUNNER" --mode "$mode" "${RUNNER_ARGS[@]}"
done

printf '[dp_microbench_matrix] PASS modes=%s\n' "${(j:,:)MODES}"
