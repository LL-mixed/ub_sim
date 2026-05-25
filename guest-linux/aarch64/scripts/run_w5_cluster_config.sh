#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_CONFIG="$ROOT_DIR/out/w5_cluster_run.env"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_cluster_config.sh [--print-env] [--steps N] [config.env]

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
STEPS_OVERRIDE=""

while (( $# > 0 )); do
  case "$1" in
    --print-env)
      PRINT_ENV=1
      shift
      ;;
    --steps)
      if (( $# < 2 )); then
        echo "--steps requires a value" >&2
        usage
        exit 2
      fi
      STEPS_OVERRIDE="$2"
      shift 2
      ;;
    --steps=*)
      STEPS_OVERRIDE="${1#--steps=}"
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

if [[ -n "$STEPS_OVERRIDE" ]]; then
  if [[ ! "$STEPS_OVERRIDE" =~ '^[0-9]+$' || "$STEPS_OVERRIDE" == "0" ]]; then
    echo "--steps must be a positive integer: $STEPS_OVERRIDE" >&2
    exit 2
  fi
  export SIM_QWEN3_GUEST_DECODE_STEPS="$STEPS_OVERRIDE"
fi

case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
  qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
    export SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-1}"
    ;;
  *)
    export SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-0}"
    ;;
esac

if (( PRINT_ENV )); then
  printf 'RUN_ID=%s\n' "${RUN_ID:-}"
  printf 'SIM_UAPI_W5_PROFILE=%s\n' "${SIM_UAPI_W5_PROFILE:-}"
  printf 'SIM_QWEN3_GUEST_ENGRAM=%s\n' "${SIM_QWEN3_GUEST_ENGRAM:-}"
  printf 'SIM_QWEN3_GUEST_DECODE_STEPS=%s\n' "${SIM_QWEN3_GUEST_DECODE_STEPS:-}"
  printf 'SIM_QWEN3_DENSE_WEIGHTS_PATH=%s\n' "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  printf 'SIM_W5_MEMORY_SHORTPATH_EXECUTE=%s\n' "${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-}"
  printf 'SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP=%s\n' "${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-}"
  printf 'SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP=%s\n' "${SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP:-}"
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
