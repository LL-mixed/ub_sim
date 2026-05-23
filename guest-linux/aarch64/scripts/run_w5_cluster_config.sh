#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_CONFIG="$ROOT_DIR/out/w5_cluster_run.env"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_cluster_config.sh [--print-env] [config.env]

Loads a W5 inference cluster env file and then runs the stable W5 cluster
entrypoint. This keeps approval prefixes stable: callers execute this script,
not a dynamically-expanded env-prefixed shell command.

RUN_ID in config files is rejected by default to avoid logappend pollution from
accidental run reuse. Set SIM_W5_ALLOW_FIXED_RUN_ID=1 only for intentional
reproduction with a manually cleaned run directory.
USAGE
}

PRINT_ENV=0
CONFIG_PATH=""

while (( $# > 0 )); do
  case "$1" in
    --print-env)
      PRINT_ENV=1
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
      if [[ -n "$CONFIG_PATH" ]]; then
        echo "only one config file may be provided" >&2
        usage
        exit 2
      fi
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

CONFIG_PATH="${CONFIG_PATH:-$DEFAULT_CONFIG}"
if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "W5 cluster config file is missing: $CONFIG_PATH" >&2
  echo "hint: write KEY=VALUE lines to the config file, then rerun this stable entrypoint" >&2
  exit 2
fi

set -a
source "$CONFIG_PATH"
set +a

if (( PRINT_ENV )); then
  printf 'RUN_ID=%s\n' "${RUN_ID:-}"
  printf 'SIM_UAPI_W5_PROFILE=%s\n' "${SIM_UAPI_W5_PROFILE:-}"
  printf 'SIM_QWEN3_GUEST_DECODE_STEPS=%s\n' "${SIM_QWEN3_GUEST_DECODE_STEPS:-}"
  printf 'SIM_QWEN3_DENSE_WEIGHTS_PATH=%s\n' "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  printf 'SIM_W5_MEMORY_SHORTPATH_EXECUTE=%s\n' "${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-}"
  printf 'SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP=%s\n' "${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-}"
  printf 'SIM_W5_MEMORY_OBSERVATION_STORE=%s\n' "${SIM_W5_MEMORY_OBSERVATION_STORE:-}"
  exit 0
fi

if [[ -n "${RUN_ID:-}" && "${SIM_W5_ALLOW_FIXED_RUN_ID:-0}" != "1" ]]; then
  echo "fixed RUN_ID is disabled for W5 cluster config runs: $RUN_ID" >&2
  echo "hint: remove RUN_ID from $CONFIG_PATH, or set SIM_W5_ALLOW_FIXED_RUN_ID=1 after manually cleaning the run directory" >&2
  exit 2
fi

echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" >&2
exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"
