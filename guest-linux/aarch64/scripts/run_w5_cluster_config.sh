#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/w5_memory_reuse_common.sh"
source "$SCRIPT_DIR/w5_model_timeout_common.sh"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_cluster_config.sh [--print-env] [--readiness-only] [--validate-only] [--managed-serving|--legacy-serving] [--model-io bulk|direct] [--numerical-trace] [--serve-queue] [--serve-requests FILE] [--nodea-ingress] [--gsva-kv] [--require-prefix-cache] [--no-memory-reuse] [--post-run-prune] [--post-run-health] [--keep-latest N] [--nodes 2|3|4|8] [--steps N] [--model PATH] [--open-euler-disk-image PATH] [--requests FILE] [--flash-payload-dir DIR] config.env

Loads a W5 inference cluster env file and then runs the stable W5 cluster
entrypoint. This keeps approval prefixes stable: callers execute this script,
not a dynamically-expanded env-prefixed shell command.

--print-env prints the effective environment grouped by runtime surface,
serving surface, and test/validation controls.

Managed serving is the default. Qwen3-0.6B and DeepSeek V4 Flash decode use
W6 direct model-kernel PTO TLOAD/TSTORE by default. --model-io bulk selects the
W5 callable 4 comparison path. Use --legacy-serving only for an explicit
legacy regression.

--numerical-trace records full terminal logits for Qwen and DeepSeek under a
run-scoped out/ directory for oracle comparison. Qwen also records per-layer
tensors. It is limited to a single managed-direct request and is excluded
from performance measurements.

RUN_ID in config files is rejected by default to avoid logappend pollution from
accidental run reuse. Set SIM_W5_ALLOW_FIXED_RUN_ID=1 only for intentional
reproduction with a manually cleaned run directory.
USAGE
}

PRINT_ENV=0
READINESS_ONLY=0
VALIDATE_ONLY=0
SERVE_QUEUE=0
MANAGED_SERVING_OVERRIDE=0
LEGACY_SERVING_OVERRIDE=0
MODEL_IO_OVERRIDE=""
NUMERICAL_TRACE_OVERRIDE=0
CONFIG_PATH=""
STEPS_OVERRIDE=""
NODES_OVERRIDE=""
KEEP_LATEST_OVERRIDE=""
REQUESTS_OVERRIDE=""
SERVE_REQUESTS_OVERRIDE=""
FLASH_PAYLOAD_DIR_OVERRIDE=""
MODEL_OVERRIDE=""
OPEN_EULER_DISK_IMAGE_OVERRIDE=""
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
    --readiness-only)
      READINESS_ONLY=1
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
    --numerical-trace)
      NUMERICAL_TRACE_OVERRIDE=1
      shift
      ;;
    --managed-serving)
      MANAGED_SERVING_OVERRIDE=1
      shift
      ;;
    --legacy-serving)
      LEGACY_SERVING_OVERRIDE=1
      shift
      ;;
    --model-io)
      if (( $# < 2 )); then
        echo "--model-io requires bulk or direct" >&2
        usage
        exit 2
      fi
      MODEL_IO_OVERRIDE="$2"
      shift 2
      ;;
    --model-io=*)
      MODEL_IO_OVERRIDE="${1#--model-io=}"
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
    --nodes)
      if (( $# < 2 )); then
        echo "--nodes requires a value" >&2
        usage
        exit 2
      fi
      NODES_OVERRIDE="$2"
      shift 2
      ;;
    --nodes=*)
      NODES_OVERRIDE="${1#--nodes=}"
      shift
      ;;
    --model)
      if (( $# < 2 )); then
        echo "--model requires a value" >&2
        usage
        exit 2
      fi
      MODEL_OVERRIDE="$2"
      shift 2
      ;;
    --model=*)
      MODEL_OVERRIDE="${1#--model=}"
      shift
      ;;
    --open-euler-disk-image)
      if (( $# < 2 )); then
        echo "--open-euler-disk-image requires a value" >&2
        usage
        exit 2
      fi
      OPEN_EULER_DISK_IMAGE_OVERRIDE="$2"
      shift 2
      ;;
    --open-euler-disk-image=*)
      OPEN_EULER_DISK_IMAGE_OVERRIDE="${1#--open-euler-disk-image=}"
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
    --flash-payload-dir)
      if (( $# < 2 )); then
        echo "--flash-payload-dir requires a value" >&2
        usage
        exit 2
      fi
      FLASH_PAYLOAD_DIR_OVERRIDE="$2"
      shift 2
      ;;
    --flash-payload-dir=*)
      FLASH_PAYLOAD_DIR_OVERRIDE="${1#--flash-payload-dir=}"
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

if [[ -z "$CONFIG_PATH" ]]; then
  echo "W5 cluster config file is required" >&2
  echo "hint: pass a config.env path, or use run_w5_cluster_qwen3_0_6b_2step.sh to generate one" >&2
  usage
  exit 2
fi
if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "W5 cluster config file is missing: $CONFIG_PATH" >&2
  echo "hint: write KEY=VALUE lines to the config file, then rerun this stable entrypoint" >&2
  exit 2
fi

set -a
source "$CONFIG_PATH"
set +a

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

export SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR="${SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR:-$SCRIPT_DIR}"

if [[ -n "$OPEN_EULER_DISK_IMAGE_OVERRIDE" ]]; then
  if [[ "$OPEN_EULER_DISK_IMAGE_OVERRIDE" != /* ]]; then
    OPEN_EULER_DISK_IMAGE_OVERRIDE="$PWD/$OPEN_EULER_DISK_IMAGE_OVERRIDE"
  fi
  export SIM_W5_OE_DISK_IMAGE="$OPEN_EULER_DISK_IMAGE_OVERRIDE"
  export SIM_W5_GUEST_ENGINE=openEuler
fi

if [[ -n "$MODEL_OVERRIDE" ]]; then
  if [[ "$MODEL_OVERRIDE" != /* ]]; then
    MODEL_OVERRIDE="$PWD/$MODEL_OVERRIDE"
  fi
  if [[ "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" == "deepseek_v4_flash_decode" ]]; then
    export SIM_DEEPSEEK_V4_FLASH="$MODEL_OVERRIDE"
  else
    export SIM_QWEN3_DENSE_WEIGHTS_PATH="$MODEL_OVERRIDE"
  fi
elif [[ -n "${SIM_DEEPSEEK_V4_FLASH:-}" && "$SIM_DEEPSEEK_V4_FLASH" != /* ]]; then
  config_dir="$(cd "$(dirname "$CONFIG_PATH")" && pwd)"
  export SIM_DEEPSEEK_V4_FLASH="$config_dir/$SIM_DEEPSEEK_V4_FLASH"
fi

reject_deprecated_w5_env_var() {
  local old_name="$1"
  local new_name="$2"
  if [[ -n "${(P)old_name:-}" ]]; then
    echo "$old_name was renamed to $new_name; update $CONFIG_PATH before running W5" >&2
    return 1
  fi
  return 0
}

reject_deprecated_w5_env() {
  local deprecated_status=0

  reject_deprecated_w5_env_var SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_DECISION_STORE SIM_W5_TEST_MEMORY_DECISION_STORE || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_DECISION_OBJECT_STORE SIM_W5_TEST_MEMORY_DECISION_OBJECT_STORE || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_OBSERVATION_STORE SIM_W5_TEST_MEMORY_OBSERVATION_STORE || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_ID || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_IDS || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID SIM_W5_TEST_MEMORY_BOUNDARY_OBSERVATION_RUN_ID || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_SHORTPATH_DECISION_ID SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_ID || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_SHORTPATH_DECISION_IDS SIM_W5_TEST_MEMORY_SHORTPATH_DECISION_IDS || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_SHORTPATH_EXECUTE SIM_W5_TEST_MEMORY_SHORTPATH_EXECUTE || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_PREFIX_CACHE_LOOKUP SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID SIM_W5_TEST_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR SIM_W5_TEST_MEMORY_PREFIX_CACHE_SERVICE_ADDR || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_POST_RUN_PROMOTE SIM_W5_TEST_MEMORY_POST_RUN_PROMOTE || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_PREFETCH_PLAN_ID SIM_W5_TEST_MEMORY_PREFETCH_PLAN_ID || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_GSVA_KV SIM_W5_TEST_MEMORY_GSVA_KV || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_MEMORY_GSVA_EXPECTED_EPOCH SIM_W5_TEST_MEMORY_GSVA_EXPECTED_EPOCH || deprecated_status=2
  reject_deprecated_w5_env_var SIM_QWEN3_PTO_TRACE_DIR SIM_W5_TEST_PTO_TRACE_DIR || deprecated_status=2
  reject_deprecated_w5_env_var SIM_W5_TEST_QWEN3_PTO_TRACE_DIR SIM_W5_TEST_PTO_TRACE_DIR || deprecated_status=2

  return "$deprecated_status"
}

if ! reject_deprecated_w5_env; then
  exit 2
fi

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
if [[ -n "$NODES_OVERRIDE" ]]; then
  case "$NODES_OVERRIDE" in
    2|3|4|8)
      export SIM_W5_CLUSTER_NODE_COUNT="$NODES_OVERRIDE"
      ;;
    *)
      echo "--nodes must be 2, 3, 4, or 8: $NODES_OVERRIDE" >&2
      exit 2
      ;;
  esac
fi
export SIM_W5_CLUSTER_NODE_COUNT="${SIM_W5_CLUSTER_NODE_COUNT:-8}"
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

if (( MANAGED_SERVING_OVERRIDE && LEGACY_SERVING_OVERRIDE )); then
  echo "--managed-serving and --legacy-serving are mutually exclusive" >&2
  exit 2
fi
if (( LEGACY_SERVING_OVERRIDE )); then
  export SIM_W5_MANAGED_SERVING=0
elif (( MANAGED_SERVING_OVERRIDE )); then
  export SIM_W5_MANAGED_SERVING=1
else
  export SIM_W5_MANAGED_SERVING="${SIM_W5_MANAGED_SERVING:-1}"
fi
if [[ -n "$MODEL_IO_OVERRIDE" ]]; then
  export SIM_W5_MODEL_IO="$MODEL_IO_OVERRIDE"
elif [[ -n "${SIM_W5_MODEL_IO:-}" ]]; then
  export SIM_W5_MODEL_IO
else
  export SIM_W5_MODEL_IO=bulk
  if bool_enabled "$SIM_W5_MANAGED_SERVING"; then
    case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
      qwen3_0_6b_decode|deepseek_v4_flash_decode)
        export SIM_W5_MODEL_IO=direct
        ;;
    esac
  fi
fi
case "$SIM_W5_MODEL_IO" in
  bulk)
    ;;
  direct)
    if ! bool_enabled "$SIM_W5_MANAGED_SERVING"; then
      echo "direct model I/O requires managed serving" >&2
      exit 2
    fi
    case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
      qwen3_0_6b_decode|qwen3_14b_decode|deepseek_v4_flash_decode)
        ;;
      *)
        echo "direct model I/O requires a Qwen or DeepSeek decode profile" >&2
        exit 2
        ;;
    esac
    ;;
  *)
    echo "SIM_W5_MODEL_IO must be bulk or direct: $SIM_W5_MODEL_IO" >&2
    exit 2
    ;;
esac

if bool_enabled "$SIM_W5_MANAGED_SERVING"; then
  case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
    qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
      echo "managed serving does not support Engram profiles; use --legacy-serving" >&2
      exit 2
      ;;
    deepseek_v4_flash_decode)
      export SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-deepseek-v4-flash-simpler}"
      ;;
    *)
      export SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}"
      ;;
  esac
  case "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" in
    qwen3_dense|deepseek-v4-flash-simpler|deepseek_v4_flash_simpler)
      ;;
    *)
      echo "managed serving does not support backend $SIM_UAPI_W4_CHIPBACKEND_PROFILE; use --legacy-serving" >&2
      exit 2
      ;;
  esac
  if (( GSVA_KV_OVERRIDE || REQUIRE_PREFIX_CACHE_OVERRIDE || DISABLE_MEMORY_REUSE_OVERRIDE )); then
    echo "managed serving cannot use legacy memory-reuse test options; use --legacy-serving for those regressions" >&2
    exit 2
  fi
  export SIM_W5_MANAGED_SERVING=1
  export SIM_LINGQU_SHMEM_PTO_ENABLE=1
  export SIM_W5_QWEN3_PTO=0
  export SIM_W5_PTO_UB_GM_PROBE=0
  export SIM_W5_PTO_UB_GM_PUBLISH_OUTPUT=0
  export SIM_QWEN3_GUEST_ENGRAM=0
  export SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP=0
  export SIM_W5_TEST_MEMORY_ONLINE_BOUNDARY_LOOKUP=0
  export SIM_W5_TEST_MEMORY_PREFIX_CACHE_LOOKUP=0
  export SIM_W5_TEST_MEMORY_GSVA_KV=0
  export SIM_W5_TEST_MEMORY_REUSE_DISABLE=1
else
  case "$SIM_W5_MANAGED_SERVING" in
    0|false|FALSE|no|NO)
      export SIM_W5_MANAGED_SERVING=0
      ;;
    *)
      echo "SIM_W5_MANAGED_SERVING must be a boolean: $SIM_W5_MANAGED_SERVING" >&2
      exit 2
      ;;
  esac
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
if (( NUMERICAL_TRACE_OVERRIDE )); then
  export SIM_W5_TEST_NUMERICAL_TRACE=1
else
  export SIM_W5_TEST_NUMERICAL_TRACE="${SIM_W5_TEST_NUMERICAL_TRACE:-0}"
fi
case "$SIM_W5_TEST_NUMERICAL_TRACE" in
  0) ;;
  1)
    if [[ -n "${SIM_W5_TEST_PTO_TRACE_DIR:-}" ]]; then
      echo "--numerical-trace creates its own run-scoped directory; remove SIM_W5_TEST_PTO_TRACE_DIR" >&2
      exit 2
    fi
    case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
      qwen3_0_6b_decode|qwen3_14b_decode|deepseek_v4_flash_decode) ;;
      *) echo "--numerical-trace requires a supported decode profile" >&2; exit 2 ;;
    esac
    if [[ "$SIM_W5_MODEL_IO" != "direct" ]] || bool_enabled "${SIM_W5_SERVING_QUEUE:-0}"; then
      echo "--numerical-trace requires single-request managed-direct inference" >&2
      exit 2
    fi
    ;;
  *) echo "SIM_W5_TEST_NUMERICAL_TRACE must be 0 or 1" >&2; exit 2 ;;
esac

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

resolve_w5_managed_fingerprint() {
  local cargo_bin repo_dir sim_cli target_dir fingerprint configured_fingerprint

  if ! bool_enabled "${SIM_W5_MANAGED_SERVING:-0}"; then
    return 0
  fi
  repo_dir="$ROOT_DIR/../.."
  sim_cli="${SIM_CLI_BIN:-$repo_dir/target/release/sim-cli}"
  if [[ -n "${SIM_CLI_BIN:-}" ]]; then
    if [[ ! -x "$sim_cli" ]]; then
      echo "managed serving SIM_CLI_BIN is not executable: $SIM_CLI_BIN" >&2
      return 2
    fi
  else
    cargo_bin="$(command -v cargo 2>/dev/null || true)"
    if [[ -z "$cargo_bin" && -x "$HOME/.cargo/bin/cargo" ]]; then
      cargo_bin="$HOME/.cargo/bin/cargo"
    fi
    if [[ -z "$cargo_bin" ]]; then
      echo "managed serving requires Cargo to build sim-cli" >&2
      return 2
    fi
    echo "[w5_cluster_config] checking sim-cli build for managed serving" >&2
    if ! (cd "$repo_dir" && "$cargo_bin" build --release -p sim-cli); then
      echo "managed serving failed to build sim-cli" >&2
      return 2
    fi
    target_dir="$(cd "$repo_dir" && "$cargo_bin" metadata --format-version=1 --no-deps | python3 -c 'import json, sys; print(json.load(sys.stdin)["target_directory"])')" || return 2
    sim_cli="$target_dir/release/sim-cli"
    if [[ ! -x "$sim_cli" ]]; then
      echo "managed serving sim-cli build did not produce an executable: $sim_cli" >&2
      return 2
    fi
    export SIM_CLI_BIN="$sim_cli"
  fi
  fingerprint="$(SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" \
    "$sim_cli" managed-model-provider-fingerprint)" || return 2
  if [[ ! "$fingerprint" =~ '^[1-9][0-9]*$' ]]; then
    echo "managed model provider returned an invalid fingerprint: $fingerprint" >&2
    return 2
  fi
  configured_fingerprint="${SIM_W5_MANAGED_ARTIFACT_FINGERPRINT:-}"
  if [[ -n "$configured_fingerprint" &&
        "$configured_fingerprint" != "0" &&
        "$configured_fingerprint" != "$fingerprint" ]]; then
    echo "refreshing stale managed artifact fingerprint: $configured_fingerprint -> $fingerprint" >&2
  fi
  export SIM_W5_MANAGED_ARTIFACT_FINGERPRINT="$fingerprint"
}

prepare_w6_direct_artifacts() {
  local artifact_profile artifact_prefix output_dir manifest fingerprint_json fingerprint sim_cli

  if [[ "${SIM_W5_MODEL_IO:-bulk}" != "direct" ]]; then
    return 0
  fi
  case "${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" in
    qwen3_0_6b_decode|qwen3_14b_decode)
      artifact_profile="host_qwen3_range"
      artifact_prefix="w6-qwen-direct-artifacts"
      ;;
    deepseek_v4_flash_decode)
      artifact_profile="host_deepseek_v4_flash_range"
      artifact_prefix="w6-deepseek-v4-flash-direct-artifacts"
      ;;
    *)
      echo "direct artifact preparation does not support profile: ${SIM_UAPI_W5_PROFILE:-}" >&2
      return 2
      ;;
  esac
  mkdir -p "$ROOT_DIR/out" || return 2
  output_dir="$(mktemp -d "$ROOT_DIR/out/${artifact_prefix}.XXXXXXXX")" || return 2
  manifest="$(python3 "$SCRIPT_DIR/prepare_simpler_host_artifacts.py" \
    --profile "$artifact_profile" \
    --output-dir "$output_dir")" || return 2
  if [[ ! -f "$manifest" ]]; then
    echo "direct artifact preparation did not produce a manifest: $manifest" >&2
    return 2
  fi
  sim_cli="${SIM_CLI_BIN:-$ROOT_DIR/../../target/release/sim-cli}"
  if [[ ! -x "$sim_cli" ]]; then
    echo "direct artifact inspection requires sim-cli: $sim_cli" >&2
    return 2
  fi
  fingerprint_json="$("$sim_cli" lingqu-shmem-pto-e2e \
    --fingerprint-manifest "$manifest")" || return 2
  fingerprint="$(printf '%s\n' "$fingerprint_json" | python3 -c \
    'import json,sys; print(json.load(sys.stdin)["artifact_fingerprint_hex"])')" || return 2
  if [[ ! "$fingerprint" =~ '^0x[0-9a-fA-F]+$' || "$fingerprint" == "0x0" ]]; then
    echo "direct manifest returned an invalid fingerprint: $fingerprint" >&2
    return 2
  fi
  if [[ "$artifact_profile" == "host_deepseek_v4_flash_range" ]]; then
    export SIMPLER_DEEPSEEK_V4_FLASH_PTO_RANGE_MANIFEST="$manifest"
  else
    export SIMPLER_QWEN3_PTO_RANGE_MANIFEST="$manifest"
  fi
  export SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT="$fingerprint"
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

is_deepseek_v4_flash_w5_profile() {
  [[ "${1:-}" == "deepseek_v4_flash_decode" ]]
}

detect_w5_aarch64_linux_cc() {
  emulate -L zsh
  setopt null_glob
  local cc
  local native_cc
  local native_target
  if [[ -n "${AARCH64_LINUX_CC:-}" ]]; then
    echo "$AARCH64_LINUX_CC"
    return 0
  fi
  native_cc="$(command -v gcc 2>/dev/null || true)"
  if [[ -n "$native_cc" ]]; then
    native_target="$($native_cc -dumpmachine 2>/dev/null || true)"
    if [[ "$native_target" == aarch64* ]]; then
      echo "$native_cc"
      return 0
    fi
  fi
  for cc in aarch64-*-gnu-gcc /usr/bin/aarch64-*-gnu-gcc /opt/homebrew/bin/aarch64-*-gnu-gcc /opt/local/bin/aarch64-*-gnu-gcc; do
    if command -v "$cc" >/dev/null 2>&1; then
      command -v "$cc"
      return 0
    fi
    if [[ -x "$cc" ]]; then
      echo "$cc"
      return 0
    fi
  done
  echo ""
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
  print_env_value SIM_W5_CLUSTER_NODE_COUNT "${SIM_W5_CLUSTER_NODE_COUNT:-8}"
  print_env_value SIM_W5_GUEST_ENGINE "${SIM_W5_GUEST_ENGINE:-initramfs}"
  print_env_value SIM_W5_OE_DISK_IMAGE "${SIM_W5_OE_DISK_IMAGE:-}"
  print_env_value SIM_QWEN3_GUEST_DECODE_STEPS "${SIM_QWEN3_GUEST_DECODE_STEPS:-}"
  print_env_value SIM_QWEN3_DENSE_WEIGHTS_PATH "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM "${SIM_QWEN3_GUEST_ENGRAM:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM_POOL "${SIM_QWEN3_GUEST_ENGRAM_POOL:-}"
  print_env_value SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP "${SIM_QWEN3_GUEST_ENGRAM_CONTEXT_OP:-}"
  print_env_value SIM_W5_PROGRESS_INTERVAL_SECS "${SIM_W5_PROGRESS_INTERVAL_SECS:-}"
  print_env_value SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE "${SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE:-}"
  print_env_value SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED "${SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED:-0}"
  print_env_value SIM_W5_MANAGED_SERVING "${SIM_W5_MANAGED_SERVING:-0}"
  print_env_value SIM_W5_MODEL_IO "${SIM_W5_MODEL_IO:-bulk}"
  print_env_value SIM_LINGQU_SHMEM_PTO_ENABLE "${SIM_LINGQU_SHMEM_PTO_ENABLE:-0}"
  print_env_value SIM_W5_QWEN3_PTO "${SIM_W5_QWEN3_PTO:-0}"
  print_env_value SIM_W5_PTO_UB_GM_PROBE "${SIM_W5_PTO_UB_GM_PROBE:-0}"
  print_env_value SIM_W5_MANAGED_ARTIFACT_FINGERPRINT "${SIM_W5_MANAGED_ARTIFACT_FINGERPRINT:-}"
  print_env_value SIMPLER_QWEN3_PTO_RANGE_MANIFEST "${SIMPLER_QWEN3_PTO_RANGE_MANIFEST:-}"
  print_env_value SIMPLER_DEEPSEEK_V4_FLASH_PTO_RANGE_MANIFEST "${SIMPLER_DEEPSEEK_V4_FLASH_PTO_RANGE_MANIFEST:-}"
  print_env_value SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT "${SIM_W5_PTO_UB_GM_ARTIFACT_FINGERPRINT:-}"
  print_env_value SIM_W5_MANAGED_CONNECT "${SIM_W5_MANAGED_CONNECT:-tcp:10.0.0.1:9700}"
  print_env_value SIM_W5_MANAGED_TIMEOUT_MS "${SIM_W5_MANAGED_TIMEOUT_MS:-}"
  print_env_value SIM_W5_PTO_UB_GM_TIMEOUT_MS "${SIM_W5_PTO_UB_GM_TIMEOUT_MS:-}"
  print_env_value SIMPLER_SCHEDULER_TIMEOUT_MS "${SIMPLER_SCHEDULER_TIMEOUT_MS:-}"
  print_env_value SIM_W5_MANAGED_PROVIDER_LEASE_MS "${SIM_W5_MANAGED_PROVIDER_LEASE_MS:-}"
  print_env_value SIM_W5_MANAGED_APERTURE_SIZE "${SIM_W5_MANAGED_APERTURE_SIZE:-0x200000000}"
  print_env_value SIM_W5_MANAGED_ALLOCATION_GRANULARITY_BYTES "${SIM_W5_MANAGED_ALLOCATION_GRANULARITY_BYTES:-67108864}"
  print_env_value SIM_W5_MANAGED_IMPORT_REGION_BYTES "${SIM_W5_MANAGED_IMPORT_REGION_BYTES:-268435456}"
  print_env_value SIM_W5_MANAGED_BOOTSTRAP_GENERATION "${SIM_W5_MANAGED_BOOTSTRAP_GENERATION:-${SIM_W5_MANAGED_PROVIDER_INCARNATION_BASE:-1000000}}"
  print_env_value SIM_W5_MANAGED_MAX_MAPPINGS "${SIM_W5_MANAGED_MAX_MAPPINGS:-8}"
  print_env_value SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR "$SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR"

  print_env_section "model"
  print_env_value SIM_UAPI_W4_CHIPBACKEND_PROFILE "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}"
  print_env_value SIM_DEEPSEEK_V4_FLASH "${SIM_DEEPSEEK_V4_FLASH:-}"
  print_env_value SIM_W5_FLASH_WEIGHT_CATALOG "${SIM_W5_FLASH_WEIGHT_CATALOG:-}"

  print_env_section "serving"
  print_env_value SIM_LLM_INFER_PROMPT "${SIM_LLM_INFER_PROMPT:-}"
  print_env_value SIM_LLM_INFER_PROMPT_TOKEN_IDS "${SIM_LLM_INFER_PROMPT_TOKEN_IDS:-}"
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

  print_env_section "test-numerical"
  print_env_value SIM_W5_TEST_NUMERICAL_TRACE "${SIM_W5_TEST_NUMERICAL_TRACE:-0}"
  print_env_value SIM_W5_TEST_PTO_TRACE_DIR "${SIM_W5_TEST_PTO_TRACE_DIR:-}"

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
  local progress_interval="${SIM_W5_PROGRESS_INTERVAL_SECS:-}"
  local keep_latest="${SIM_W5_TEST_ARTIFACT_KEEP_LATEST:-3}"
  local max_prune_candidates="${SIM_W5_TEST_HEALTH_MAX_PRUNE_CANDIDATES:-0}"
  local max_prune_bytes="${SIM_W5_TEST_HEALTH_MAX_PRUNE_BYTES:-0}"
  local cluster_node_count="${SIM_W5_CLUSTER_NODE_COUNT:-8}"

  if [[ "$SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR" != /* ]]; then
    echo "SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR must be an absolute directory: $SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR" >&2
    return 2
  fi
  if [[ ! -f "$SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR/prepare_simpler_host_artifacts.py" ]]; then
    echo "SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR is missing prepare_simpler_host_artifacts.py: $SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR" >&2
    return 2
  fi
  if [[ ! -f "$SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR/prepare_simpler_host_vector_artifacts.py" ]]; then
    echo "SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR is missing prepare_simpler_host_vector_artifacts.py: $SIMPLER_HOST_ARTIFACTS_SCRIPT_DIR" >&2
    return 2
  fi

  case "$profile" in
    qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode|deepseek_v4_flash_decode)
      ;;
    *)
      echo "unsupported SIM_UAPI_W5_PROFILE=$profile" >&2
      return 2
      ;;
  esac
  case "$cluster_node_count" in
    2|3|4|8)
      ;;
    *)
      echo "SIM_W5_CLUSTER_NODE_COUNT must be 2, 3, 4, or 8: $cluster_node_count" >&2
      return 2
      ;;
  esac
  if [[ ! "$steps" =~ '^[0-9]+$' || "$steps" == "0" ]]; then
    echo "SIM_QWEN3_GUEST_DECODE_STEPS must be a positive integer: $steps" >&2
    return 2
  fi
  if bool_enabled "${SIM_W5_MANAGED_SERVING:-0}"; then
    case "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" in
      qwen3_dense|deepseek-v4-flash-simpler|deepseek_v4_flash_simpler)
        ;;
      *)
        echo "managed serving requires qwen3_dense or DeepSeek simpler backend: ${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-unset}" >&2
        return 2
        ;;
    esac
    if ! bool_enabled "${SIM_LINGQU_SHMEM_PTO_ENABLE:-0}" ||
       bool_enabled "${SIM_W5_QWEN3_PTO:-0}" ||
       bool_enabled "${SIM_W5_PTO_UB_GM_PROBE:-0}" ||
       bool_enabled "${SIM_QWEN3_GUEST_ENGRAM:-0}" ||
       [[ ! "${SIM_W5_MANAGED_ARTIFACT_FINGERPRINT:-}" =~ '^[1-9][0-9]*$' ]]; then
      echo "managed serving configuration is incomplete or conflicts with a legacy model path" >&2
      return 2
    fi
  fi
  if [[ -n "$progress_interval" && ! "$progress_interval" =~ '^[0-9]+$' ]]; then
    echo "SIM_W5_PROGRESS_INTERVAL_SECS must be a non-negative integer: $progress_interval" >&2
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
  if ! is_deepseek_v4_flash_w5_profile "$profile" && [[ -z "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" ]]; then
    echo "W5 cluster config requires SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    return 2
  fi
  if is_deepseek_v4_flash_w5_profile "$profile" &&
     [[ -n "${SIM_DEEPSEEK_V4_FLASH:-}" ]] &&
     [[ ! -f "$SIM_DEEPSEEK_V4_FLASH" && ! -d "$SIM_DEEPSEEK_V4_FLASH" ]]; then
    echo "SIM_DEEPSEEK_V4_FLASH model source is missing: $SIM_DEEPSEEK_V4_FLASH" >&2
    return 2
  fi
  if [[ "${SIM_W5_GUEST_ENGINE:-initramfs}" == "openEuler" ]] &&
     [[ ! -f "${SIM_W5_OE_DISK_IMAGE:-}" ]]; then
    echo "SIM_W5_OE_DISK_IMAGE is missing: ${SIM_W5_OE_DISK_IMAGE:-unset}" >&2
    return 2
  fi
  if [[ "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" == "deepseek-v4-flash-official" ||
        "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" == "deepseek_v4_flash_official" ]]; then
    if [[ ! -d "${SIM_DEEPSEEK_V4_FLASH:-}" ||
          ! -f "${SIM_DEEPSEEK_V4_FLASH:-}/config.json" ||
          ! -f "${SIM_DEEPSEEK_V4_FLASH:-}/model.safetensors.index.json" ]]; then
      echo "official DeepSeek profile requires a checkpoint directory with config.json and model.safetensors.index.json: ${SIM_DEEPSEEK_V4_FLASH:-unset}" >&2
      return 2
    fi
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
  if ! is_deepseek_v4_flash_w5_profile "$profile" && [[ ! -d "$SIM_QWEN3_DENSE_WEIGHTS_PATH" ]]; then
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

run_w5_readiness_checks() {
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local steps="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"

  if is_deepseek_v4_flash_w5_profile "$profile"; then
    local guest_link_cc
    local flash_route_trace="crates/sim-models/fixtures/deepseek_v4_flash_route_trace.ds4.txt"
    local default_flash_route_manifest="crates/sim-models/fixtures/deepseek_v4_flash_route_trace.manifest.txt"
    local flash_route_manifest="${SIM_W5_TEST_FLASH_ROUTE_TRACE_MANIFEST:-$default_flash_route_manifest}"
    local flash_weight_provider="crates/sim-models/fixtures/deepseek_v4_flash_weight_provider.fixture.txt"
    local default_flash_file_weight_provider="crates/sim-models/fixtures/deepseek_v4_flash_weight_provider.file.fixture.txt"
    local flash_file_weight_provider="${SIM_W5_TEST_FLASH_WEIGHT_PROVIDER:-$default_flash_file_weight_provider}"
    local flash_weight_catalog="${SIM_W5_FLASH_WEIGHT_CATALOG:-${SIM_W5_TEST_FLASH_WEIGHT_CATALOG:-}}"
    local generated_flash_weight_catalog=""
    local flash_weight_catalog_tmpdir=""
    local flash_payload_dir=""
    local flash_route_source_kind="fixture"
    if [[ -n "${SIM_W5_TEST_FLASH_ROUTE_TRACE_MANIFEST:-}" ]]; then
      flash_route_source_kind="ds4-measured"
    fi
    if [[ -n "$FLASH_PAYLOAD_DIR_OVERRIDE" && -n "$flash_weight_catalog" ]]; then
      echo "--flash-payload-dir cannot be combined with SIM_W5_FLASH_WEIGHT_CATALOG or SIM_W5_TEST_FLASH_WEIGHT_CATALOG" >&2
      return 2
    fi
    if [[ -n "$FLASH_PAYLOAD_DIR_OVERRIDE" && ! -d "$FLASH_PAYLOAD_DIR_OVERRIDE" ]]; then
      echo "DeepSeek V4 Flash payload dir is missing: $FLASH_PAYLOAD_DIR_OVERRIDE" >&2
      return 2
    fi
    if [[ -n "$FLASH_PAYLOAD_DIR_OVERRIDE" ]]; then
      flash_payload_dir="$(cd "$FLASH_PAYLOAD_DIR_OVERRIDE" && pwd)"
    fi
    guest_link_cc="$(detect_w5_aarch64_linux_cc)"
    if [[ -z "$guest_link_cc" ]]; then
      echo "DeepSeek V4 Flash readiness requires AARCH64_LINUX_CC or aarch64-*-gnu-gcc on PATH for guest link verification" >&2
      return 2
    fi
    (
      cd "$ROOT_DIR"
      AARCH64_LINUX_CC="$guest_link_cc" "$SCRIPT_DIR/build_initramfs.sh" --w5-guest-link-only
    )
    if [[ "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" == "deepseek-v4-flash-official" ||
          "${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}" == "deepseek_v4_flash_official" ]]; then
      (
        cd "$ROOT_DIR/../.."
        cargo run --release -p sim-models --bin deepseek_v4_flash_checkpoint -- \
          validate --model "$SIM_DEEPSEEK_V4_FLASH" >/dev/null
      )
      return 0
    fi
    if [[ -z "$flash_weight_catalog" ]]; then
      flash_weight_catalog_tmpdir="$(mktemp -d "${TMPDIR:-/tmp}/w5_flash_weight_catalog.XXXXXX")"
      generated_flash_weight_catalog="$flash_weight_catalog_tmpdir/weight.catalog"
      flash_weight_catalog="$generated_flash_weight_catalog"
      if [[ -n "$flash_payload_dir" ]]; then
        local layer_id
        for layer_id in {0..42}; do
          ln -s "$flash_payload_dir/layer$layer_id" "$flash_weight_catalog_tmpdir/layer$layer_id"
        done
      fi
    fi
    if ! (
      cd "$ROOT_DIR/../.."
      if [[ -n "$generated_flash_weight_catalog" ]]; then
        if [[ -n "$flash_payload_dir" ]]; then
          cargo run --release -p sim-cli -- deepseek-v4-flash-weight-catalog \
            --payload-dir "$flash_payload_dir" \
            --source-kind "$flash_route_source_kind" \
            --output "$generated_flash_weight_catalog" >/dev/null
        else
          cargo run --release -p sim-cli -- deepseek-v4-flash-weight-catalog \
            --from-provider "$flash_file_weight_provider" \
            --source-kind "$flash_route_source_kind" \
            --output "$generated_flash_weight_catalog" >/dev/null
        fi
      fi
      cargo run --release -p sim-cli -- qwen3-decode-loop \
        --scenario=8host \
        --steps="$steps" \
        --profile=deepseek-v4-flash >/dev/null
      cargo run --release -p sim-cli -- deepseek-v4-flash-moe-report \
        --steps="$steps" >/dev/null
      cargo run --release -p sim-cli -- deepseek-v4-flash-moe-report \
        --route-trace "$flash_route_trace" \
        --weight-provider "$flash_weight_provider" >/dev/null
      cargo run --release -p sim-cli -- deepseek-v4-flash-moe-report \
        --route-trace-manifest "$flash_route_manifest" \
        --require-route-source-kind "$flash_route_source_kind" \
        --weight-catalog "$flash_weight_catalog" >/dev/null
    ); then
      [[ -n "$flash_weight_catalog_tmpdir" ]] && rm -rf "$flash_weight_catalog_tmpdir"
      return 1
    fi
    [[ -n "$flash_weight_catalog_tmpdir" ]] && rm -rf "$flash_weight_catalog_tmpdir"
  fi
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

w5_memory_runtime_needs_bootstrap() {
  if bool_enabled "${SIM_W5_MANAGED_SERVING:-0}"; then
    return 1
  fi
  if bool_enabled "${SIM_W5_TEST_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1}"; then
    return 0
  fi
  if [[ -n "${SIM_W5_TEST_MEMORY_DECISION_STORE:-}" ]]; then
    return 0
  fi
  return 1
}

w5_ensure_run_id() {
  if [[ -n "${RUN_ID:-}" ]]; then
    return 0
  fi
  RUN_ID="$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}_${RANDOM}"
  export RUN_ID
}

bootstrap_w5_memory_service_infra() {
  if ! w5_memory_runtime_needs_bootstrap; then
    return 0
  fi
  if bool_enabled "${SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED:-0}"; then
    return 0
  fi

  w5_ensure_run_id
  export SIM_W5_MEMORY_STORE="${SIM_W5_MEMORY_STORE:-$ROOT_DIR/out/w5_memory_object_store.${RUN_ID}.json}"
  export SIM_W5_MEMORY_OBJECT_STORE="${SIM_W5_MEMORY_OBJECT_STORE:-$ROOT_DIR/out/w5_object_service_store.${RUN_ID}.json}"
  export SIM_W5_MEMORY_ENGRAM_STATE="${SIM_W5_MEMORY_ENGRAM_STATE:-$ROOT_DIR/out/w5_memory_engram_state.${RUN_ID}.json}"
  export SIM_W5_MEMORY_REGISTRY_DIR="${SIM_W5_MEMORY_REGISTRY_DIR:-$ROOT_DIR/out/w5_memory_registry.${RUN_ID}}"
  export SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE="${SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE:-$ROOT_DIR/out/w5_memory_service_env.${RUN_ID}.sh}"

  "$SCRIPT_DIR/run_w5_memory_service_bootstrap.sh" \
    --env-file "$SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE"
  source "$SIM_W5_MEMORY_BOOTSTRAP_ENV_FILE"

  if ! bool_enabled "${SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED:-0}"; then
    echo "W5 Memory Service bootstrap did not report SIM_W5_MEMORY_SERVICE_BOOTSTRAPPED=1" >&2
    return 2
  fi
}

resolve_w5_managed_timeout_status=0
w5_resolve_managed_timeout_ms || resolve_w5_managed_timeout_status=$?
if (( resolve_w5_managed_timeout_status != 0 )); then
  exit "$resolve_w5_managed_timeout_status"
fi

resolve_w5_direct_pto_timeout_status=0
w5_resolve_direct_pto_timeout_ms || resolve_w5_direct_pto_timeout_status=$?
if (( resolve_w5_direct_pto_timeout_status != 0 )); then
  exit "$resolve_w5_direct_pto_timeout_status"
fi

resolve_w5_direct_scheduler_timeout_status=0
w5_resolve_direct_scheduler_timeout_ms || resolve_w5_direct_scheduler_timeout_status=$?
if (( resolve_w5_direct_scheduler_timeout_status != 0 )); then
  exit "$resolve_w5_direct_scheduler_timeout_status"
fi

resolve_w5_managed_provider_lease_status=0
w5_resolve_managed_provider_lease_ms || resolve_w5_managed_provider_lease_status=$?
if (( resolve_w5_managed_provider_lease_status != 0 )); then
  exit "$resolve_w5_managed_provider_lease_status"
fi

resolve_w5_managed_fingerprint_status=0
resolve_w5_managed_fingerprint || resolve_w5_managed_fingerprint_status=$?
if (( resolve_w5_managed_fingerprint_status != 0 )); then
  exit "$resolve_w5_managed_fingerprint_status"
fi
prepare_w6_direct_artifacts_status=0
prepare_w6_direct_artifacts || prepare_w6_direct_artifacts_status=$?
if (( prepare_w6_direct_artifacts_status != 0 )); then
  exit "$prepare_w6_direct_artifacts_status"
fi

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

if (( READINESS_ONLY )); then
  run_w5_readiness_checks
  echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode} readiness_only=1"
  exit 0
fi

bootstrap_w5_memory_service_infra_status=0
bootstrap_w5_memory_service_infra || bootstrap_w5_memory_service_infra_status=$?
if (( bootstrap_w5_memory_service_infra_status != 0 )); then
  exit "$bootstrap_w5_memory_service_infra_status"
fi

if (( VALIDATE_ONLY )); then
  echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode} validate_only=1" >&2
  export SIM_W5_TEST_VALIDATE_ONLY=1
  if [[ -n "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}" ]]; then
    unset SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG
  fi
  exec "$SCRIPT_DIR/run_w5_inference_cluster_runtime.sh"
fi

echo "[w5_cluster_config] config=$CONFIG_PATH profile=${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}" >&2
if [[ -n "${SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}" ]]; then
  unset SIM_W5_TEST_MEMORY_REUSE_RUN_ID_FOR_DEBUG
fi
if bool_enabled "${SIM_W5_TEST_POST_RUN_PRUNE:-0}" || bool_enabled "${SIM_W5_TEST_POST_RUN_HEALTH:-0}"; then
  "$SCRIPT_DIR/run_w5_inference_cluster_runtime.sh"
  run_post_run_maintenance
else
  exec "$SCRIPT_DIR/run_w5_inference_cluster_runtime.sh"
fi
