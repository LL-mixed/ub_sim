#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_CONFIG="$ROOT_DIR/out/w5_cluster_run.env"
source "$SCRIPT_DIR/w5_memory_reuse_common.sh"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_cluster_config.sh [--print-env] [--validate-only] [--serve-queue] [--serve-requests FILE] [--nodea-ingress] [--gsva-kv] [--require-prefix-cache] [--no-memory-reuse] [--post-run-prune] [--post-run-health] [--keep-latest N] [--steps N] [--requests FILE] [config.env]

Loads a W5 inference cluster env file and then runs the stable W5 cluster
entrypoint. This keeps approval prefixes stable: callers execute this script,
not a dynamically-expanded env-prefixed shell command.

--print-env prints the effective environment grouped by runtime surface,
serving surface, and test/validation controls.

RUN_ID in config files is rejected by default to avoid logappend pollution from
accidental run reuse. Set SIM_W5_ALLOW_FIXED_RUN_ID=1 only for intentional
reproduction with a manually cleaned run directory.
USAGE
}

PRINT_ENV=0
VALIDATE_ONLY=0
SERVE_QUEUE=0
CONFIG_PATH=""
STEPS_OVERRIDE=""
KEEP_LATEST_OVERRIDE=""
REQUESTS_OVERRIDE=""
SERVE_REQUESTS_OVERRIDE=""
NODEA_INGRESS_OVERRIDE=0
GSVA_KV_OVERRIDE=0
REQUIRE_PREFIX_CACHE_OVERRIDE=0
DISABLE_MEMORY_REUSE_OVERRIDE=0
SIM_W5_TEST_REQUIRE_PREFIX_CACHE="${SIM_W5_TEST_REQUIRE_PREFIX_CACHE:-0}"

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
    --serve-queue)
      SERVE_QUEUE=1
      shift
      ;;
    --serve-requests)
      if (( $# < 2 )); then
        echo "--serve-requests requires a value" >&2
        usage
        exit 2
      fi
      SERVE_QUEUE=1
      SERVE_REQUESTS_OVERRIDE="$2"
      shift 2
      ;;
    --serve-requests=*)
      SERVE_QUEUE=1
      SERVE_REQUESTS_OVERRIDE="${1#--serve-requests=}"
      shift
      ;;
    --nodea-ingress)
      SERVE_QUEUE=1
      NODEA_INGRESS_OVERRIDE=1
      shift
      ;;
    --gsva-kv)
      GSVA_KV_OVERRIDE=1
      shift
      ;;
    --require-prefix-cache)
      REQUIRE_PREFIX_CACHE_OVERRIDE=1
      shift
      ;;
    --no-memory-reuse)
      DISABLE_MEMORY_REUSE_OVERRIDE=1
      shift
      ;;
    --post-run-prune)
      export SIM_W5_TEST_POST_RUN_PRUNE=1
      export SIM_W5_TEST_POST_RUN_HEALTH="${SIM_W5_TEST_POST_RUN_HEALTH:-1}"
      shift
      ;;
    --post-run-health)
      export SIM_W5_TEST_POST_RUN_HEALTH=1
      shift
      ;;
    --keep-latest)
      if (( $# < 2 )); then
        echo "--keep-latest requires a value" >&2
        usage
        exit 2
      fi
      KEEP_LATEST_OVERRIDE="$2"
      shift 2
      ;;
    --keep-latest=*)
      KEEP_LATEST_OVERRIDE="${1#--keep-latest=}"
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
    --requests)
      if (( $# < 2 )); then
        echo "--requests requires a value" >&2
        usage
        exit 2
      fi
      REQUESTS_OVERRIDE="$2"
      shift 2
      ;;
    --requests=*)
      REQUESTS_OVERRIDE="${1#--requests=}"
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

if [[ -n "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID:-}" ]]; then
  echo "SIM_W5_TEST_MEMORY_REUSE_RUN_ID was renamed to SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG; normal W5 runs auto-discover reusable Memory Service stores without this variable" >&2
  exit 2
fi
if [[ -n "$STEPS_OVERRIDE" ]]; then
  if [[ ! "$STEPS_OVERRIDE" =~ '^[0-9]+$' || "$STEPS_OVERRIDE" == "0" ]]; then
    echo "--steps must be a positive integer: $STEPS_OVERRIDE" >&2
    exit 2
  fi
  export SIM_QWEN3_GUEST_DECODE_STEPS="$STEPS_OVERRIDE"
fi
if [[ -n "$KEEP_LATEST_OVERRIDE" ]]; then
  if [[ ! "$KEEP_LATEST_OVERRIDE" =~ '^[0-9]+$' ]]; then
    echo "--keep-latest must be a non-negative integer: $KEEP_LATEST_OVERRIDE" >&2
    exit 2
  fi
  export SIM_W5_TEST_ARTIFACT_KEEP_LATEST="$KEEP_LATEST_OVERRIDE"
fi
if (( GSVA_KV_OVERRIDE )); then
  export SIM_W5_TEST_MEMORY_GSVA_KV=1
fi
if (( REQUIRE_PREFIX_CACHE_OVERRIDE )); then
  export SIM_W5_TEST_REQUIRE_PREFIX_CACHE=1
fi
if (( DISABLE_MEMORY_REUSE_OVERRIDE )); then
  export SIM_W5_TEST_MEMORY_REUSE_DISABLE=1
fi
if [[ -n "$REQUESTS_OVERRIDE" ]]; then
  export SIM_W5_SERVING_REQUESTS_FILE="$REQUESTS_OVERRIDE"
fi
if (( SERVE_QUEUE )); then
  export SIM_W5_SERVING_QUEUE=1
fi
if [[ -n "$SERVE_REQUESTS_OVERRIDE" ]]; then
  export SIM_W5_SERVING_SUBMIT_REQUESTS_FILE="$SERVE_REQUESTS_OVERRIDE"
fi
if (( NODEA_INGRESS_OVERRIDE )); then
  export SIM_W5_SERVING_INGRESS=nodeA
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

context_guard_requires() {
  local required="$1"
  local raw="${SIM_W5_TEST_REQUIRE_CONTEXT:-}"
  local normalized=""
  [[ -n "$raw" ]] || return 1
  normalized="${raw//$'\n'/,}"
  normalized="${normalized// /,}"
  case ",$normalized," in
    *,"$required",*)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

print_env_section() {
  printf '# %s\n' "$1"
}

print_env_value() {
  local name="$1"
  local value="$2"
  printf '%s=%s\n' "$name" "$value"
}

print_w5_effective_env() {
  print_env_section "runtime"
  print_env_value RUN_ID "${RUN_ID:-}"
  print_env_value SIM_UAPI_W5_PROFILE "${SIM_UAPI_W5_PROFILE:-}"
  print_env_value SIM_QWEN3_GUEST_DECODE_STEPS "${SIM_QWEN3_GUEST_DECODE_STEPS:-}"
  print_env_value SIM_QWEN3_DENSE_WEIGHTS_PATH "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM "${SIM_QWEN3_GUEST_ENGRAM:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM_POOL "${SIM_QWEN3_GUEST_ENGRAM_POOL:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP "${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-}"

  print_env_section "serving"
  print_env_value SIM_W5_SERVING_REQUESTS_FILE "${SIM_W5_SERVING_REQUESTS_FILE:-}"
  print_env_value SIM_W5_SERVING_QUEUE "${SIM_W5_SERVING_QUEUE:-0}"
  print_env_value SIM_W5_SERVING_INGRESS "${SIM_W5_SERVING_INGRESS:-cluster}"
  print_env_value SIM_W5_SERVING_SUBMIT_REQUESTS_FILE "${SIM_W5_SERVING_SUBMIT_REQUESTS_FILE:-}"

  print_env_section "test-memory-reuse"
  print_env_value SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE "${SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE:-}"
  print_env_value SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP "${SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1}"
  print_env_value SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP "${SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP:-1}"
  print_env_value SIM_W5_TEST_MEMORY_GSVA_KV "${SIM_W5_TEST_MEMORY_GSVA_KV:-}"
  print_env_value SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE "${SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE:-}"
  print_env_value SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP "${SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP:-}"
  print_env_value SIM_W5_TEST_MEMORY_OBSERVATION_STORE "${SIM_W5_TEST_MEMORY_OBSERVATION_STORE:-}"
  print_env_value SIM_W5_TEST_VALIDATE_ONLY "${SIM_W5_TEST_VALIDATE_ONLY:-}"
  print_env_value SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}"
  print_env_value SIM_W5_TEST_MEMORY_REUSE_DISABLE "${SIM_W5_TEST_MEMORY_REUSE_DISABLE:-0}"
  print_env_value SIM_W5_TEST_MEMORY_REUSE_OUT_DIR "${SIM_W5_TEST_MEMORY_REUSE_OUT_DIR:-}"
  print_env_value SIM_W5_TEST_MEMORY_DECISION_STORE "${SIM_W5_TEST_MEMORY_DECISION_STORE:-}"
  print_env_value SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE "${SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE:-}"
  print_env_value SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID "${SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}"
  print_env_value SIM_W5_TEST_REQUIRE_CONTEXT "${SIM_W5_TEST_REQUIRE_CONTEXT:-}"
  print_env_value SIM_W5_TEST_REQUIRE_PREFIX_CACHE "${SIM_W5_TEST_REQUIRE_PREFIX_CACHE:-0}"

  print_env_section "test-maintenance"
  print_env_value SIM_W5_TEST_POST_RUN_PRUNE "${SIM_W5_TEST_POST_RUN_PRUNE:-}"
  print_env_value SIM_W5_TEST_POST_RUN_HEALTH "${SIM_W5_TEST_POST_RUN_HEALTH:-}"
  print_env_value SIM_W5_TEST_ARTIFACT_KEEP_LATEST "${SIM_W5_TEST_ARTIFACT_KEEP_LATEST:-3}"
  print_env_value SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES "${SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES:-0}"
  print_env_value SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES "${SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES:-0}"

  print_env_section "vendor-context-test"
  print_env_value SIM_ENGRAM_SIMT_ARTIFACT_DIR "${SIM_ENGRAM_SIMT_ARTIFACT_DIR:-}"
  print_env_value SIM_ENGRAM_SIMT_SELECTED_SYMBOL "${SIM_ENGRAM_SIMT_SELECTED_SYMBOL:-}"
  print_env_value SIM_ENGRAM_SIMT_SELECTED_CASE "${SIM_ENGRAM_SIMT_SELECTED_CASE:-}"
  print_env_value SIM_ENGRAM_SIMT_BINARY_PATH "${SIM_ENGRAM_SIMT_BINARY_PATH:-}"
  print_env_value SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH "${SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH:-}"
}

validate_w5_cluster_config() {
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local steps="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
  local memory_runtime_lookup=0
  local memory_online_lookup=0
  local memory_prefix_cache_lookup=1
  local memory_require_prefix_cache=0
  local serving_ingress="${SIM_W5_SERVING_INGRESS:-cluster}"
  local keep_latest="${SIM_W5_TEST_ARTIFACT_KEEP_LATEST:-3}"
  local max_prune_candidates="${SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES:-0}"
  local max_prune_bytes="${SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES:-0}"

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
  if [[ ! "$keep_latest" =~ '^[0-9]+$' ]]; then
    echo "SIM_W5_TEST_ARTIFACT_KEEP_LATEST must be a non-negative integer: $keep_latest" >&2
    return 2
  fi
  if [[ ! "$max_prune_candidates" =~ '^[0-9]+$' ]]; then
    echo "SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES must be a non-negative integer: $max_prune_candidates" >&2
    return 2
  fi
  if [[ ! "$max_prune_bytes" =~ '^[0-9]+$' ]]; then
    echo "SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES must be a non-negative integer: $max_prune_bytes" >&2
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
  if bool_enabled "${SIM_W5_SERVING_QUEUE:-0}" &&
     { bool_enabled "${SIM_W5_TEST_POST_RUN_PRUNE:-0}" || bool_enabled "${SIM_W5_TEST_POST_RUN_HEALTH:-0}"; }; then
    echo "SIM_W5_SERVING_QUEUE cannot be combined with post-run maintenance" >&2
    return 2
  fi
  if [[ -n "${SIM_W5_SERVING_SUBMIT_REQUESTS_FILE:-}" ]]; then
    if ! bool_enabled "${SIM_W5_SERVING_QUEUE:-0}"; then
      echo "SIM_W5_SERVING_SUBMIT_REQUESTS_FILE requires SIM_W5_SERVING_QUEUE=1" >&2
      return 2
    fi
    if [[ ! -f "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" ]]; then
      echo "W5 serving submit request file is missing: $SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" >&2
      return 2
    fi
    if ! "$SCRIPT_DIR/w5_serving_entry.py" --requests "$SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" --validate-only >/dev/null; then
      echo "W5 serving submit request file is invalid: $SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" >&2
      return 2
    fi
  fi
  case "$serving_ingress" in
    cluster|nodeA)
      ;;
    *)
      echo "SIM_W5_SERVING_INGRESS must be cluster or nodeA: $serving_ingress" >&2
      return 2
      ;;
  esac
  if [[ "$serving_ingress" == "nodeA" ]]; then
    if ! bool_enabled "${SIM_W5_SERVING_QUEUE:-0}"; then
      echo "SIM_W5_SERVING_INGRESS=nodeA requires SIM_W5_SERVING_QUEUE=1" >&2
      return 2
    fi
    if [[ -z "${SIM_W5_SERVING_SUBMIT_REQUESTS_FILE:-}" ]]; then
      echo "SIM_W5_SERVING_INGRESS=nodeA requires SIM_W5_SERVING_SUBMIT_REQUESTS_FILE" >&2
      return 2
    fi
  fi
  if [[ -n "${SIM_W5_SERVING_REQUESTS_FILE:-}" ]]; then
    if bool_enabled "${SIM_W5_SERVING_QUEUE:-0}"; then
      echo "SIM_W5_SERVING_QUEUE cannot be combined with SIM_W5_SERVING_REQUESTS_FILE" >&2
      return 2
    fi
    if [[ ! -f "$SIM_W5_SERVING_REQUESTS_FILE" ]]; then
      echo "W5 serving request file is missing: $SIM_W5_SERVING_REQUESTS_FILE" >&2
      return 2
    fi
    if ! "$SCRIPT_DIR/w5_serving_entry.py" --requests "$SIM_W5_SERVING_REQUESTS_FILE" --validate-only >/dev/null; then
      echo "W5 serving request file is invalid: $SIM_W5_SERVING_REQUESTS_FILE" >&2
      return 2
    fi
  fi
  if [[ ! -d "$SIM_QWEN3_DENSE_WEIGHTS_PATH" ]]; then
    echo "W5 cluster config weights path is missing: $SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    return 2
  fi
  if bool_enabled "${SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1}"; then
    memory_runtime_lookup=1
  fi
  if bool_enabled "${SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP:-0}"; then
    memory_online_lookup=1
  fi
  case "${SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP:-1}" in
    0|false|FALSE|no|NO)
      memory_prefix_cache_lookup=0
      ;;
  esac
  case "${SIM_W5_TEST_REQUIRE_PREFIX_CACHE:-0}" in
    1|true|TRUE|yes|YES)
      memory_require_prefix_cache=1
      ;;
  esac
  if [[ -n "${SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE:-}" && -z "${SIM_W5_TEST_MEMORY_DECISION_STORE:-}" ]]; then
    echo "SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE requires SIM_W5_TEST_MEMORY_DECISION_STORE" >&2
    return 2
  fi
  if [[ -n "${SIM_W5_TEST_MEMORY_DECISION_STORE:-}" ]]; then
    if [[ -z "${SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_ID:-}" &&
          -z "${SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" &&
          -z "${SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" &&
          -z "${SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID:-}" &&
          -z "${SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_IDS:-}" &&
          "$memory_online_lookup" == "0" &&
          "$memory_prefix_cache_lookup" == "0" &&
           -z "${SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID:-}" &&
           -z "${SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID:-}" &&
           -z "${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}" &&
           -z "${SIM_W5_TEST_MEMORY_SHORTPATH_STREAM_PATH:-}" ]]; then
      echo "SIM_W5_TEST_MEMORY_DECISION_STORE requires a boundary observation/decision selector or enabled prefix-cache lookup for live Memory Service reuse" >&2
      return 2
    fi
    if [[ ! -f "$SIM_W5_TEST_MEMORY_DECISION_STORE" ]]; then
      echo "Memory Service decision store is missing: $SIM_W5_TEST_MEMORY_DECISION_STORE" >&2
      return 2
    fi
  fi
  if [[ -n "${SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE:-}" ]]; then
    if [[ ! -f "$SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE" ]]; then
      echo "Memory Service decision object store is missing: $SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE" >&2
      return 2
    fi
  fi
  if (( memory_require_prefix_cache )) && (( memory_prefix_cache_lookup == 0 )); then
    echo "SIM_W5_TEST_REQUIRE_PREFIX_CACHE requires SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=1" >&2
    return 2
  fi
  if (( memory_runtime_lookup )) && [[ -n "${SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID:-}${SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
    echo "SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP cannot be combined with explicit shortpath decision ids" >&2
    return 2
  fi
  if context_guard_requires "fused_simt_vendor_context"; then
    if ! bool_enabled "${SIM_QWEN3_GUEST_ENGRAM:-0}"; then
      echo "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context requires SIM_QWEN3_GUEST_ENGRAM=1" >&2
      return 2
    fi
    if [[ "${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-}" != "fused-simt" ]]; then
      echo "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context requires SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP=fused-simt" >&2
      return 2
    fi
    if [[ -n "${SIM_ENGRAM_SIMT_ARTIFACT_DIR:-}" ]]; then
      if [[ ! -d "$SIM_ENGRAM_SIMT_ARTIFACT_DIR" ]]; then
        echo "SIM_ENGRAM_SIMT_ARTIFACT_DIR is missing: $SIM_ENGRAM_SIMT_ARTIFACT_DIR" >&2
        return 2
      elif [[ ! -f "$SIM_ENGRAM_SIMT_ARTIFACT_DIR/engram-simt" ]]; then
        echo "SIM_ENGRAM_SIMT_ARTIFACT_DIR is missing engram-simt: $SIM_ENGRAM_SIMT_ARTIFACT_DIR" >&2
        return 2
      elif [[ ! -f "$SIM_ENGRAM_SIMT_ARTIFACT_DIR/libengram-simt_kernel.so" ]]; then
        echo "SIM_ENGRAM_SIMT_ARTIFACT_DIR is missing libengram-simt_kernel.so: $SIM_ENGRAM_SIMT_ARTIFACT_DIR" >&2
        return 2
      fi
    elif [[ -z "${SIM_ENGRAM_SIMT_SELECTED_SYMBOL:-}" ||
            -z "${SIM_ENGRAM_SIMT_SELECTED_CASE:-}" ||
            -z "${SIM_ENGRAM_SIMT_BINARY_PATH:-}" ||
            -z "${SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH:-}" ]]; then
      echo "SIM_W5_TEST_REQUIRE_CONTEXT=fused_simt_vendor_context requires SIM_ENGRAM_SIMT_ARTIFACT_DIR or complete SIM_ENGRAM_SIMT_SELECTED_* vendor env" >&2
      return 2
    elif [[ ! -f "$SIM_ENGRAM_SIMT_BINARY_PATH" ]]; then
      echo "SIM_ENGRAM_SIMT_BINARY_PATH is missing: $SIM_ENGRAM_SIMT_BINARY_PATH" >&2
      return 2
    elif [[ ! -f "$SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH" ]]; then
      echo "SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH is missing: $SIM_ENGRAM_SIMT_KERNEL_LIBRARY_PATH" >&2
      return 2
    fi
  fi
  return 0
}

run_post_run_maintenance() {
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local keep_latest="${SIM_W5_TEST_ARTIFACT_KEEP_LATEST:-3}"
  local max_prune_candidates="${SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES:-0}"
  local max_prune_bytes="${SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES:-0}"

  if bool_enabled "${SIM_W5_TEST_POST_RUN_PRUNE:-0}"; then
    echo "[w5_cluster_config] post_run_prune=1 profile=$profile keep_latest=$keep_latest" >&2
    "$SCRIPT_DIR/w5_artifact_prune.py" \
      --profile "$profile" \
      --keep-latest "$keep_latest" \
      --summary-only \
      --delete
  fi
  if bool_enabled "${SIM_W5_TEST_POST_RUN_HEALTH:-0}"; then
    echo "[w5_cluster_config] post_run_health=1 profile=$profile keep_latest=$keep_latest" >&2
    "$SCRIPT_DIR/w5_cluster_health_check.py" \
      --profile "$profile" \
      --keep-latest "$keep_latest" \
      --max-prune-candidates "$max_prune_candidates" \
      --max-prune-bytes "$max_prune_bytes"
  fi
}

resolve_w5_memory_reuse_config_status=0
w5_resolve_memory_reuse_config "$ROOT_DIR/out" "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" "${SIM_QWEN3_GUEST_DECODE_STEPS:-1}" || resolve_w5_memory_reuse_config_status=$?
if (( resolve_w5_memory_reuse_config_status != 0 )); then
  exit "$resolve_w5_memory_reuse_config_status"
fi

if (( PRINT_ENV )); then
  print_w5_effective_env
  exit 0
fi

validate_w5_cluster_config_status=0
validate_w5_cluster_config || validate_w5_cluster_config_status=$?
if (( validate_w5_cluster_config_status != 0 )); then
  exit "$validate_w5_cluster_config_status"
fi

if (( VALIDATE_ONLY )); then
  echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode} validate_only=1" >&2
  export SIM_W5_TEST_VALIDATE_ONLY=1
  if [[ -n "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}" ]]; then
    unset SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG
  fi
  exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"
fi

echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" >&2
if [[ -n "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}" ]]; then
  unset SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG
fi
if bool_enabled "${SIM_W5_TEST_POST_RUN_PRUNE:-0}" || bool_enabled "${SIM_W5_TEST_POST_RUN_HEALTH:-0}"; then
  "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"
  run_post_run_maintenance
else
  exec "$SCRIPT_DIR/run_ub_eight_node_w5_inference_cluster.sh"
fi
