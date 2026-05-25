#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_CONFIG="$ROOT_DIR/out/w5_cluster_run.env"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_cluster_config.sh [--print-env] [--validate-only] [--steps N] [config.env]

Loads a W5 inference cluster env file and then runs the stable W5 cluster
entrypoint. This keeps approval prefixes stable: callers execute this script,
not a dynamically-expanded env-prefixed shell command.

RUN_ID in config files is rejected by default to avoid logappend pollution from
accidental run reuse. Set SIM_W5_ALLOW_FIXED_RUN_ID=1 only for intentional
reproduction with a manually cleaned run directory.
USAGE
}

PRINT_ENV=0
VALIDATE_ONLY=0
CONFIG_PATH=""
STEPS_OVERRIDE=""

while (( $# > 0 )); do
  case "$1" in
    --print-env)
      PRINT_ENV=1
      shift
      ;;
    --validate-only)
      VALIDATE_ONLY=1
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
    export SIM_QWEN3_GUEST_ENGRAM_POOL="${SIM_QWEN3_GUEST_ENGRAM_POOL:-obmm}"
    ;;
  *)
    export SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-0}"
    export SIM_QWEN3_GUEST_ENGRAM_POOL="${SIM_QWEN3_GUEST_ENGRAM_POOL:-}"
    ;;
esac

bool_enabled() {
  case "${1:-}" in
    1|true|TRUE|yes|YES)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

resolve_w5_memory_reuse_config() {
  local reuse_run_id="${SIM_W5_MEMORY_REUSE_RUN_ID:-}"
  if [[ -z "$reuse_run_id" ]]; then
    return 0
  fi
  if [[ -n "${SIM_W5_MEMORY_DECISION_STORE:-}" ||
        -n "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
    echo "SIM_W5_MEMORY_REUSE_RUN_ID cannot be combined with explicit Memory Service reuse stores or selectors" >&2
    return 2
  fi

  local reuse_out_dir="${SIM_W5_MEMORY_REUSE_OUT_DIR:-$ROOT_DIR/out}"
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local decision_store=""
  local selected_run_id="$reuse_run_id"

  if [[ "$reuse_run_id" == "latest" ]]; then
    local -a candidates
    candidates=("$reuse_out_dir"/w5_memory_runtime_boundary_lookup.*_w5_${profile}_*.json(N.om[1]))
    if (( ${#candidates[@]} == 0 )); then
      echo "SIM_W5_MEMORY_REUSE_RUN_ID=latest found no decision store for profile=$profile in $reuse_out_dir" >&2
      return 2
    fi
    decision_store="${candidates[1]}"
    local base="${decision_store:t}"
    selected_run_id="${base#w5_memory_runtime_boundary_lookup.}"
    selected_run_id="${selected_run_id%.json}"
  else
    if [[ ! "$reuse_run_id" =~ '^[A-Za-z0-9._-]+$' ]]; then
      echo "SIM_W5_MEMORY_REUSE_RUN_ID must be latest or a run id without path separators: $reuse_run_id" >&2
      return 2
    fi
    decision_store="$reuse_out_dir/w5_memory_runtime_boundary_lookup.$selected_run_id.json"
  fi

  local object_store="$reuse_out_dir/w5_object_service_store.$selected_run_id.json"
  if [[ ! -f "$decision_store" ]]; then
    echo "W5 Memory Service reuse decision store is missing: $decision_store" >&2
    return 2
  fi
  if [[ ! -f "$object_store" ]]; then
    echo "W5 Memory Service reuse object store is missing: $object_store" >&2
    return 2
  fi

  export SIM_W5_MEMORY_DECISION_STORE="$decision_store"
  export SIM_W5_MEMORY_DECISION_OBJECT_STORE="$object_store"
  export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID="$selected_run_id"
  export SIM_W5_MEMORY_SHORTPATH_EXECUTE="${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-1}"
}

validate_w5_cluster_config() {
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local steps="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
  local memory_runtime_lookup=0
  local memory_online_lookup=0

  case "$profile" in
    qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      ;;
    *)
      echo "unsupported SIM_UAPI_W5_PROFILE=$profile" >&2
      return 2
      ;;
  esac
  if [[ ! "$steps" =~ '^[0-9]+$' || "$steps" == "0" ]]; then
    echo "SIM_QWEN3_GUEST_DECODE_STEPS must be a positive integer: $steps" >&2
    return 2
  fi
  if [[ -n "${RUN_ID:-}" && "${SIM_W5_ALLOW_FIXED_RUN_ID:-0}" != "1" ]]; then
    echo "fixed RUN_ID is disabled for W5 cluster config runs: $RUN_ID" >&2
    echo "hint: remove RUN_ID from $CONFIG_PATH, or set SIM_W5_ALLOW_FIXED_RUN_ID=1 after manually cleaning the run directory" >&2
    return 2
  fi
  if [[ -z "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" ]]; then
    echo "W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    return 2
  fi
  if [[ ! -d "$SIM_QWEN3_DENSE_WEIGHTS_PATH" ]]; then
    echo "W5 cluster config weights path is missing: $SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    return 2
  fi
  if bool_enabled "${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-0}"; then
    memory_runtime_lookup=1
  fi
  if bool_enabled "${SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP:-0}"; then
    memory_online_lookup=1
  fi
  if [[ -n "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}" && -z "${SIM_W5_MEMORY_DECISION_STORE:-}" ]]; then
    echo "SIM_W5_MEMORY_DECISION_OBJECT_STORE requires SIM_W5_MEMORY_DECISION_STORE" >&2
    return 2
  fi
  if [[ -n "${SIM_W5_MEMORY_DECISION_STORE:-}" ]]; then
    if [[ -z "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}" &&
          -z "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" &&
          -z "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" &&
          -z "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}" &&
          -z "${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" &&
          "$memory_online_lookup" == "0" &&
          -z "${SIM_W5_MEMORY_PREFETCH_PLAN_ID:-}" &&
          -z "${SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID:-}" &&
          -z "${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}" &&
          -z "${SIM_W5_MEMORY_SHORTPATH_STREAM_PATH:-}" ]]; then
      echo "SIM_W5_MEMORY_DECISION_STORE requires a boundary observation/decision selector for live Memory Service reuse" >&2
      return 2
    fi
  fi
  if (( memory_runtime_lookup )) && [[ -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
    echo "SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP cannot be combined with explicit shortpath decision ids" >&2
    return 2
  fi
  return 0
}

resolve_w5_memory_reuse_config_status=0
resolve_w5_memory_reuse_config || resolve_w5_memory_reuse_config_status=$?
if (( resolve_w5_memory_reuse_config_status != 0 )); then
  exit "$resolve_w5_memory_reuse_config_status"
fi

if (( PRINT_ENV )); then
  printf 'RUN_ID=%s\n' "${RUN_ID:-}"
  printf 'SIM_UAPI_W5_PROFILE=%s\n' "${SIM_UAPI_W5_PROFILE:-}"
  printf 'SIM_QWEN3_GUEST_ENGRAM=%s\n' "${SIM_QWEN3_GUEST_ENGRAM:-}"
  printf 'SIM_QWEN3_GUEST_ENGRAM_POOL=%s\n' "${SIM_QWEN3_GUEST_ENGRAM_POOL:-}"
  printf 'SIM_QWEN3_GUEST_DECODE_STEPS=%s\n' "${SIM_QWEN3_GUEST_DECODE_STEPS:-}"
  printf 'SIM_QWEN3_DENSE_WEIGHTS_PATH=%s\n' "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  printf 'SIM_W5_MEMORY_SHORTPATH_EXECUTE=%s\n' "${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-}"
  printf 'SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP=%s\n' "${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-}"
  printf 'SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP=%s\n' "${SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP:-}"
  printf 'SIM_W5_MEMORY_OBSERVATION_STORE=%s\n' "${SIM_W5_MEMORY_OBSERVATION_STORE:-}"
  printf 'SIM_W5_MEMORY_REUSE_RUN_ID=%s\n' "${SIM_W5_MEMORY_REUSE_RUN_ID:-}"
  printf 'SIM_W5_MEMORY_REUSE_OUT_DIR=%s\n' "${SIM_W5_MEMORY_REUSE_OUT_DIR:-}"
  printf 'SIM_W5_MEMORY_DECISION_STORE=%s\n' "${SIM_W5_MEMORY_DECISION_STORE:-}"
  printf 'SIM_W5_MEMORY_DECISION_OBJECT_STORE=%s\n' "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}"
  printf 'SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=%s\n' "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}"
  exit 0
fi

validate_w5_cluster_config_status=0
validate_w5_cluster_config || validate_w5_cluster_config_status=$?
if (( validate_w5_cluster_config_status != 0 )); then
  exit "$validate_w5_cluster_config_status"
fi

if (( VALIDATE_ONLY )); then
  echo "[w5_cluster_config] config validation passed: $CONFIG_PATH" >&2
  exit 0
fi

echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" >&2
if [[ -n "${SIM_W5_MEMORY_REUSE_RUN_ID:-}" ]]; then
  unset SIM_W5_MEMORY_REUSE_RUN_ID
fi
exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"
