#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_PATH="$ROOT_DIR/out/w5_cluster_qwen3_0_6b_2step.matrix.env"
STEPS=8
REUSE_RUNS=2
DRY_RUN=0

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_prefix_cache_realistic_matrix.sh [--steps N] [--reuse-runs N] [--dry-run] [config.env]

Runs a W5 prefix-cache reuse matrix:
  1. seed run with GSVA KV writeback and memory reuse disabled
  2. N GSVA-backed prefix-cache reuse runs that must hit prefix cache
  3. benefit comparison for each reuse run against the seed run

Defaults: --steps 8 --reuse-runs 2.
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --steps)
      if (( $# < 2 )); then
        echo "--steps requires a value" >&2
        usage
        exit 2
      fi
      STEPS="$2"
      shift 2
      ;;
    --steps=*)
      STEPS="${1#--steps=}"
      shift
      ;;
    --reuse-runs)
      if (( $# < 2 )); then
        echo "--reuse-runs requires a value" >&2
        usage
        exit 2
      fi
      REUSE_RUNS="$2"
      shift 2
      ;;
    --reuse-runs=*)
      REUSE_RUNS="${1#--reuse-runs=}"
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "unsupported option: $1" >&2
      usage
      exit 2
      ;;
    *)
      if [[ -n "${CONFIG_ARG_SEEN:-}" ]]; then
        echo "only one config file may be provided" >&2
        usage
        exit 2
      fi
      CONFIG_ARG_SEEN=1
      CONFIG_PATH="$1"
      shift
      ;;
  esac
done

if (( $# > 0 )); then
  echo "unexpected trailing arguments: $*" >&2
  usage
  exit 2
fi
if [[ ! "$STEPS" =~ '^[0-9]+$' || "$STEPS" == "0" ]]; then
  echo "--steps must be a positive integer: $STEPS" >&2
  exit 2
fi
if [[ ! "$REUSE_RUNS" =~ '^[0-9]+$' || "$REUSE_RUNS" == "0" ]]; then
  echo "--reuse-runs must be a positive integer: $REUSE_RUNS" >&2
  exit 2
fi
if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "W5 cluster config file is missing: $CONFIG_PATH" >&2
  exit 2
fi

set -a
source "$CONFIG_PATH"
set +a

PROFILE="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
OUT_DIR="$ROOT_DIR/out"
RUNNER="$SCRIPT_DIR/run_w5_cluster_config.sh"
REPORT="$SCRIPT_DIR/w5_inference_run_report.py"
SUMMARY_GLOB="eight_node_w5_inference_cluster_summary.*_w5_${PROFILE}_*.txt"

latest_summary() {
  local -a summaries
  summaries=("$OUT_DIR"/$~SUMMARY_GLOB(N.om[1]))
  if (( ${#summaries[@]} == 0 )); then
    echo "missing W5 summary for profile=$PROFILE in $OUT_DIR" >&2
    return 1
  fi
  echo "${summaries[1]}"
}

run_case() {
  local label="$1"
  shift
  echo "[w5_prefix_cache_realistic_matrix] run=$label steps=$STEPS profile=$PROFILE"
  if (( DRY_RUN )); then
    printf '%q ' "$RUNNER" --steps "$STEPS" --gsva-kv "$@" "$CONFIG_PATH"
    printf '\n'
    return 0
  fi
  "$RUNNER" --steps "$STEPS" --gsva-kv "$@" "$CONFIG_PATH"
}

echo "=== W5 Prefix Cache Realistic Matrix ==="
echo "Profile:    $PROFILE"
echo "Steps:      $STEPS"
echo "Reuse runs: $REUSE_RUNS"
echo "Config:     $CONFIG_PATH"

run_case seed --no-memory-reuse
if (( DRY_RUN )); then
  seed_summary="$OUT_DIR/eight_node_w5_inference_cluster_summary.<seed-run>.txt"
else
  seed_summary="$(latest_summary)"
fi
echo "[w5_prefix_cache_realistic_matrix] seed_summary=$seed_summary"

reuse_index=1
while (( reuse_index <= REUSE_RUNS )); do
  run_case "reuse-$reuse_index" --require-prefix-cache
  if (( DRY_RUN )); then
    reuse_summary="$OUT_DIR/eight_node_w5_inference_cluster_summary.<reuse-$reuse_index-run>.txt"
    printf '%q ' "$REPORT" --compare-prefix-cache-benefit "$seed_summary" "$reuse_summary"
    printf '\n'
  else
    reuse_summary="$(latest_summary)"
    echo "[w5_prefix_cache_realistic_matrix] reuse_summary=$reuse_summary"
    "$REPORT" --compare-prefix-cache-benefit "$seed_summary" "$reuse_summary"
  fi
  (( reuse_index += 1 ))
done

echo "[w5_prefix_cache_realistic_matrix] PASS"
