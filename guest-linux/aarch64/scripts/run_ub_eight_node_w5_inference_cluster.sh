#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
source "$SCRIPT_DIR/w5_memory_reuse_common.sh"

SIM_UAPI_W5_PROFILE="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"

case "$SIM_UAPI_W5_PROFILE" in
  qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
    ;;
  *)
    echo "unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE" >&2
    exit 2
    ;;
esac

case "$SIM_UAPI_W5_PROFILE" in
  qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
    SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-1}"
    SIM_QWEN3_GUEST_ENGRAM_POOL="${SIM_QWEN3_GUEST_ENGRAM_POOL:-obmm}"
    ;;
  *)
    SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-0}"
    SIM_QWEN3_GUEST_ENGRAM_POOL="${SIM_QWEN3_GUEST_ENGRAM_POOL:-}"
    ;;
esac

RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE}_${RANDOM}}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w5_inference_cluster.trace.latest.txt}"
RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w5_inference_cluster_summary.${RUN_ID}.txt}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}"
SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP="${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-1}"
SIM_W5_MEMORY_POST_RUN_PROMOTE="${SIM_W5_MEMORY_POST_RUN_PROMOTE:-0}"
SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP="${SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP:-0}"
SIM_W5_MEMORY_OBSERVATION_STORE="${SIM_W5_MEMORY_OBSERVATION_STORE:-}"
SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG="${SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}"
SIM_W5_MEMORY_REUSE_OUT_DIR="${SIM_W5_MEMORY_REUSE_OUT_DIR:-$OUT_DIR}"
SIM_W5_MEMORY_DECISION_STORE="${SIM_W5_MEMORY_DECISION_STORE:-}"
SIM_W5_MEMORY_DECISION_OBJECT_STORE="${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}"
SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID="${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}"
SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS="${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}"
SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID="${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}"
SIM_W5_MEMORY_SHORTPATH_DECISION_ID="${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}"
SIM_W5_MEMORY_SHORTPATH_DECISION_IDS="${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}"
SIM_W5_MEMORY_PREFETCH_PLAN_ID="${SIM_W5_MEMORY_PREFETCH_PLAN_ID:-}"
SIM_W5_MEMORY_PREFIX_CACHE_LOOKUP="${SIM_W5_MEMORY_PREFIX_CACHE_LOOKUP:-1}"
SIM_W5_REQUIRE_PREFIX_CACHE="${SIM_W5_REQUIRE_PREFIX_CACHE:-0}"
SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID="${SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID:-}"
SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR="${SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR:-}"
SIM_W5_MEMORY_GSVA_KV="${SIM_W5_MEMORY_GSVA_KV:-}"
SIM_W5_MEMORY_GSVA_EXPECTED_EPOCH="${SIM_W5_MEMORY_GSVA_EXPECTED_EPOCH:-}"
SIM_W5_SERVING_QUEUE="${SIM_W5_SERVING_QUEUE:-0}"
SIM_W5_SERVING_INGRESS="${SIM_W5_SERVING_INGRESS:-cluster}"
SIM_W5_SERVING_SUBMIT_REQUESTS_FILE="${SIM_W5_SERVING_SUBMIT_REQUESTS_FILE:-}"
SIM_W5_MEMORY_STORE="${SIM_W5_MEMORY_STORE:-$OUT_DIR/w5_memory_object_store.${RUN_ID}.json}"
SIM_W5_MEMORY_OBJECT_STORE="${SIM_W5_MEMORY_OBJECT_STORE:-$OUT_DIR/w5_object_service_store.${RUN_ID}.json}"
SIM_W5_MEMORY_ENGRAM_STATE="${SIM_W5_MEMORY_ENGRAM_STATE:-$OUT_DIR/w5_memory_engram_state.${RUN_ID}.json}"
SIM_W5_MEMORY_REGISTRY_DIR="${SIM_W5_MEMORY_REGISTRY_DIR:-$OUT_DIR/w5_memory_registry.${RUN_ID}}"
SIM_W5_MEMORY_OWNER_ENTITY="${SIM_W5_MEMORY_OWNER_ENTITY:-0}"
SIM_W5_MEMORY_PRODUCER_ENTITY="${SIM_W5_MEMORY_PRODUCER_ENTITY:-0}"
SIM_W5_SHORTPATH_MATCH_MODE="${SIM_W5_SHORTPATH_MATCH_MODE:-}"
SIM_W5_MIN_MATCH_SCORE_MILLI="${SIM_W5_MIN_MATCH_SCORE_MILLI:-}"
SIM_W5_MIN_TERMINAL_MARGIN_MILLI="${SIM_W5_MIN_TERMINAL_MARGIN_MILLI:-}"
SIM_W5_MIN_SOURCE_CONFIDENCE_MILLI="${SIM_W5_MIN_SOURCE_CONFIDENCE_MILLI:-}"
SIM_W5_APPROXIMATE_REQUIRES_VERIFY="${SIM_W5_APPROXIMATE_REQUIRES_VERIFY:-1}"
SIM_W5_VALIDATE_ONLY="${SIM_W5_VALIDATE_ONLY:-0}"

export RUN_ID
export TRACE_FILE
export RUN_SUMMARY_FILE
export SIM_UAPI_W5_PROFILE
export SIM_UAPI_W4_CHIPBACKEND_PROFILE
export SIM_QWEN3_GUEST_ENGRAM
export SIM_QWEN3_GUEST_ENGRAM_POOL
export SIM_QWEN3_GUEST_ENGRAM_STATE_REF="${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}"
export SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION="${SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION:-}"
export SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF="${SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF:-}"
export SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR="${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}"
export SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT="${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}"
export SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP
export SIM_W5_MEMORY_POST_RUN_PROMOTE
export SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP
export SIM_W5_MEMORY_OBSERVATION_STORE
export SIM_W5_REQUIRE_PREFIX_CACHE
export SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG
export SIM_W5_MEMORY_REUSE_OUT_DIR
export SIM_W5_MEMORY_DECISION_STORE
export SIM_W5_MEMORY_DECISION_OBJECT_STORE
export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID
export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS
export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID
export SIM_W5_MEMORY_SHORTPATH_DECISION_ID
export SIM_W5_MEMORY_SHORTPATH_DECISION_IDS
export SIM_W5_MEMORY_PREFETCH_PLAN_ID
export SIM_W5_MEMORY_PREFIX_CACHE_LOOKUP
export SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID
export SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR
export SIM_W5_MEMORY_GSVA_KV
export SIM_W5_MEMORY_GSVA_EXPECTED_EPOCH
export SIM_W5_SERVING_QUEUE
export SIM_W5_SERVING_INGRESS
export SIM_W5_SERVING_SUBMIT_REQUESTS_FILE
export SIM_W5_MEMORY_STORE
export SIM_W5_MEMORY_OBJECT_STORE
export SIM_W5_MEMORY_ENGRAM_STATE
export SIM_W5_MEMORY_REGISTRY_DIR
export SIM_W5_MEMORY_OWNER_ENTITY
export SIM_W5_MEMORY_PRODUCER_ENTITY
export SIM_W5_SHORTPATH_MATCH_MODE
export SIM_W5_MIN_MATCH_SCORE_MILLI
export SIM_W5_MIN_TERMINAL_MARGIN_MILLI
export SIM_W5_MIN_SOURCE_CONFIDENCE_MILLI
export SIM_W5_APPROXIMATE_REQUIRES_VERIFY
export SIM_W5_VALIDATE_ONLY

memory_runtime_lookup=0
if [[ "$SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP" == "1" ||
      "$SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP" == "true" ]]; then
  memory_runtime_lookup=1
fi

memory_online_lookup=0
if [[ "$SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP" == "1" ||
      "$SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP" == "true" ]]; then
  memory_online_lookup=1
fi

memory_validate_only=0
case "$SIM_W5_VALIDATE_ONLY" in
  1|true|TRUE|yes|YES)
    memory_validate_only=1
    ;;
esac

memory_prefix_cache_lookup=0
case "$SIM_W5_MEMORY_PREFIX_CACHE_LOOKUP" in
  0|false|FALSE|no|NO)
    memory_prefix_cache_lookup=0
    ;;
  *)
    memory_prefix_cache_lookup=1
    ;;
esac

resolve_w5_memory_reuse_config_status=0
w5_resolve_memory_reuse_config "$OUT_DIR" "$SIM_UAPI_W5_PROFILE" "${SIM_QWEN3_GUEST_DECODE_STEPS:-1}" || resolve_w5_memory_reuse_config_status=$?
if (( resolve_w5_memory_reuse_config_status != 0 )); then
  exit "$resolve_w5_memory_reuse_config_status"
fi

memory_decision_reuse=0
if [[ -n "$SIM_W5_MEMORY_DECISION_STORE" ]]; then
  if [[ -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID" ||
        -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS" ||
        -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID" ||
        -n "$SIM_W5_MEMORY_SHORTPATH_DECISION_ID" ||
        -n "$SIM_W5_MEMORY_SHORTPATH_DECISION_IDS" ||
        "$memory_online_lookup" == "1" ||
        -n "$SIM_W5_MEMORY_PREFETCH_PLAN_ID" ||
        "$memory_prefix_cache_lookup" == "1" ||
        -n "$SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID" ]]; then
    memory_decision_reuse=1
  elif [[ -z "${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}" &&
          -z "${SIM_W5_MEMORY_SHORTPATH_STREAM_PATH:-}" ]]; then
    echo "SIM_W5_MEMORY_DECISION_STORE requires a boundary observation/decision selector for live Memory Service reuse" >&2
    echo "hint: set SIM_W5_MEMORY_ONLINE_BOUNDARY_LOOKUP=1, SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID, SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID(S), or SIM_W5_MEMORY_SHORTPATH_DECISION_ID(S)" >&2
    exit 2
  fi
fi

explicit_engram_state_ref=0
if [[ -n "$SIM_QWEN3_GUEST_ENGRAM_STATE_REF" ]]; then
  explicit_engram_state_ref=1
fi
if [[ -n "$SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF" && -z "$SIM_QWEN3_GUEST_ENGRAM_STATE_REF" ]]; then
  echo "SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF requires SIM_QWEN3_GUEST_ENGRAM_STATE_REF" >&2
  exit 2
fi
if (( explicit_engram_state_ref && (memory_runtime_lookup || memory_decision_reuse) )); then
  echo "SIM_QWEN3_GUEST_ENGRAM_STATE_REF cannot be combined with Memory Service bootstrap/reuse" >&2
  echo "hint: use either explicit paper ENGRAM_STATE object refs, or let Memory Service publish/materialize the state ref" >&2
  exit 2
fi

if (( memory_runtime_lookup || memory_decision_reuse || explicit_engram_state_ref )); then
  if [[ -z "${SIM_CLI_BIN:-}" ]]; then
    SIM_CLI_BIN="$REPO_DIR/target/debug/sim-cli"
    echo "[w5_inference_cluster] build sim-cli for current workspace: $SIM_CLI_BIN" >&2
    pushd "$REPO_DIR" >/dev/null
    cargo build -p sim-cli
    popd >/dev/null
  fi
  if [[ ! -x "$SIM_CLI_BIN" ]]; then
    echo "W5 sim-cli orchestration path requires sim-cli: $SIM_CLI_BIN" >&2
    echo "hint: set SIM_CLI_BIN to a built sim-cli, or unset SIM_CLI_BIN so the runner builds the workspace default" >&2
    exit 2
  fi
  if [[ -z "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" ]]; then
    echo "W5 sim-cli orchestration path requires SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    exit 2
  fi
  if (( memory_runtime_lookup )) && [[ -z "$SIM_W5_MEMORY_OBSERVATION_STORE" ]]; then
    SIM_W5_MEMORY_OBSERVATION_STORE="$OUT_DIR/w5_memory_runtime_boundary_lookup.${RUN_ID}.json"
    export SIM_W5_MEMORY_OBSERVATION_STORE
  fi

  launch_prefix_cache_service() {
    local service_addr_file="$OUT_DIR/w5_prefix_cache_service_ready.${RUN_ID}.txt"
    local service_log_file="$OUT_DIR/w5_prefix_cache_service.${RUN_ID}.log"
    local service_object_store="$SIM_W5_MEMORY_OBJECT_STORE"
    local pid

    if [[ -n "$SIM_W5_MEMORY_DECISION_OBJECT_STORE" ]]; then
      service_object_store="$SIM_W5_MEMORY_DECISION_OBJECT_STORE"
    fi

    if [[ -z "$SIM_W5_MEMORY_DECISION_STORE" ]]; then
      return
    fi

    if [[ -n "$SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR" ]]; then
      return
    fi

    rm -f "$service_addr_file"
    (
      "${SIM_CLI_BIN}" lingqu-memory prefix-cache-service \
        --store "$SIM_W5_MEMORY_DECISION_STORE" \
        --object-store "$service_object_store" \
        --ready-file "$service_addr_file" \
        --addr 127.0.0.1:0 \
        >"$service_log_file" 2>&1
    ) &
    pid=$!

    if [[ "$pid" == "0" || -z "$pid" ]]; then
      echo "failed to launch prefix-cache service" >&2
      return 1
    fi
    SIM_W5_PREFIX_CACHE_SERVICE_PID=$pid
    export SIM_W5_PREFIX_CACHE_SERVICE_PID

    local deadline=$((SECONDS + 20))
    while (( SECONDS < deadline )); do
      if [[ -s "$service_addr_file" ]]; then
        SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR="$(<"$service_addr_file")"
        export SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR
        return 0
      fi
      sleep 0.2
    done

    kill "$pid" >/dev/null 2>&1 || true
    wait "$pid" >/dev/null 2>&1 || true
    if [[ -f "$service_log_file" ]]; then
      echo "prefix-cache service failed to report ready; last log:" >&2
      tail -n 40 "$service_log_file" >&2
    fi
    echo "prefix-cache service did not become ready" >&2
    return 1
  }

  stop_prefix_cache_service() {
    if [[ -n "${SIM_W5_PREFIX_CACHE_SERVICE_PID:-}" ]] && ps -p "$SIM_W5_PREFIX_CACHE_SERVICE_PID" >/dev/null 2>&1; then
      kill "$SIM_W5_PREFIX_CACHE_SERVICE_PID" >/dev/null 2>&1 || true
      wait "$SIM_W5_PREFIX_CACHE_SERVICE_PID" >/dev/null 2>&1 || true
    fi
  }

  trap 'stop_prefix_cache_service' EXIT

  cli_args=(
    w5-inference-cluster
    --script "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
    --w5-profile "$SIM_UAPI_W5_PROFILE"
    --steps "${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
    --weights-path "$SIM_QWEN3_DENSE_WEIGHTS_PATH"
    --prompt-token-ids "${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-81378,37585,374}"
  )
  case "$SIM_W5_VALIDATE_ONLY" in
    1|true|TRUE|yes|YES)
      cli_args+=(--validate-only)
      ;;
  esac
  if (( memory_runtime_lookup )); then
    cli_args+=(
      --memory-observation-store "$SIM_W5_MEMORY_OBSERVATION_STORE"
      --memory-runtime-boundary-lookup
      --memory-store "$SIM_W5_MEMORY_STORE"
      --memory-object-store "$SIM_W5_MEMORY_OBJECT_STORE"
      --memory-engram-state "$SIM_W5_MEMORY_ENGRAM_STATE"
      --memory-registry-dir "$SIM_W5_MEMORY_REGISTRY_DIR"
      --memory-owner-entity "$SIM_W5_MEMORY_OWNER_ENTITY"
      --memory-producer-entity "$SIM_W5_MEMORY_PRODUCER_ENTITY"
    )
    case "$SIM_W5_MEMORY_POST_RUN_PROMOTE" in
      1|true|TRUE|yes|YES)
        cli_args+=(--memory-post-run-promote)
        ;;
    esac
    if [[ -n "$SIM_W5_SHORTPATH_MATCH_MODE" ]]; then
      cli_args+=(--shortpath-match-mode "$SIM_W5_SHORTPATH_MATCH_MODE")
    fi
    if [[ -n "$SIM_W5_MIN_MATCH_SCORE_MILLI" ]]; then
      cli_args+=(--min-match-score-milli "$SIM_W5_MIN_MATCH_SCORE_MILLI")
    fi
    if [[ -n "$SIM_W5_MIN_TERMINAL_MARGIN_MILLI" ]]; then
      cli_args+=(--min-terminal-margin-milli "$SIM_W5_MIN_TERMINAL_MARGIN_MILLI")
    fi
    case "$SIM_W5_APPROXIMATE_REQUIRES_VERIFY" in
      0|false|FALSE|no|NO)
        cli_args+=(--approximate-requires-verify=false)
        ;;
    esac
    if [[ -n "$SIM_W5_MIN_SOURCE_CONFIDENCE_MILLI" ]]; then
      cli_args+=(--min-source-confidence-milli "$SIM_W5_MIN_SOURCE_CONFIDENCE_MILLI")
    fi
  fi
  if (( memory_decision_reuse )); then
    cli_args+=(
      --memory-store "$SIM_W5_MEMORY_STORE"
      --memory-object-store "$SIM_W5_MEMORY_OBJECT_STORE"
      --memory-engram-state "$SIM_W5_MEMORY_ENGRAM_STATE"
      --memory-registry-dir "$SIM_W5_MEMORY_REGISTRY_DIR"
      --memory-owner-entity "$SIM_W5_MEMORY_OWNER_ENTITY"
      --memory-producer-entity "$SIM_W5_MEMORY_PRODUCER_ENTITY"
      --memory-decision-store "$SIM_W5_MEMORY_DECISION_STORE"
    )
    if [[ -n "$SIM_W5_MEMORY_DECISION_OBJECT_STORE" ]]; then
      cli_args+=(--memory-decision-object-store "$SIM_W5_MEMORY_DECISION_OBJECT_STORE")
    fi
    if [[ -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID" ]]; then
      cli_args+=(--memory-boundary-observation-id "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID")
    fi
    if [[ -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS" ]]; then
      cli_args+=(--memory-boundary-observation-ids "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS")
    fi
    if [[ -n "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID" ]]; then
      cli_args+=(--memory-boundary-observation-run-id "$SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID")
    fi
    if [[ -n "$SIM_W5_MEMORY_SHORTPATH_DECISION_ID" ]]; then
      cli_args+=(--memory-shortpath-decision-id "$SIM_W5_MEMORY_SHORTPATH_DECISION_ID")
    fi
    if [[ -n "$SIM_W5_MEMORY_SHORTPATH_DECISION_IDS" ]]; then
      cli_args+=(--memory-shortpath-decision-ids "$SIM_W5_MEMORY_SHORTPATH_DECISION_IDS")
    fi
    if (( memory_online_lookup )); then
      cli_args+=(--memory-online-boundary-lookup)
    fi
    if [[ -n "$SIM_W5_MEMORY_PREFETCH_PLAN_ID" ]]; then
      cli_args+=(--memory-prefetch-plan-id "$SIM_W5_MEMORY_PREFETCH_PLAN_ID")
    fi
    if (( memory_prefix_cache_lookup )); then
      cli_args+=(--memory-prefix-cache-lookup=true)
    else
      cli_args+=(--memory-prefix-cache-lookup=false)
    fi
    if [[ -n "$SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID" ]]; then
      cli_args+=(--memory-prefix-cache-reuse-plan-id "$SIM_W5_MEMORY_PREFIX_CACHE_REUSE_PLAN_ID")
    fi
    case "${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-1}" in
      0|false|FALSE|no|NO)
        cli_args+=(--memory-shortpath-execute=false)
      ;;
      *)
        cli_args+=(--memory-shortpath-execute=true)
        ;;
    esac
  fi
  if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
    cli_args+=(--engram --engram-pool "$SIM_QWEN3_GUEST_ENGRAM_POOL")
    if [[ -n "${SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION:-}" ]]; then
      cli_args+=(--engram-token-projection "$SIM_QWEN3_GUEST_ENGRAM_TOKENIZER_PROJECTION")
    fi
  fi
  if [[ -n "${SIM_QWEN3_GUEST_ENGRAM_STATE_REF:-}" ]]; then
    cli_args+=(--engram-state-ref "$SIM_QWEN3_GUEST_ENGRAM_STATE_REF")
    if [[ -n "${SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF:-}" ]]; then
      cli_args+=(--engram-row-prefetch-ref "$SIM_QWEN3_GUEST_ENGRAM_ROW_PREFETCH_REF")
    fi
    if [[ -n "${SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR:-}" ]]; then
      cli_args+=(--object-registry-dir "$SIM_UAPI_QWEN3_OBJECT_REGISTRY_DIR")
    fi
    if [[ -n "${SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT:-}" ]]; then
      cli_args+=(--object-service-snapshot "$SIM_UAPI_QWEN3_OBJECT_SERVICE_SNAPSHOT")
    fi
  fi
  if [[ -n "${SIM_QWEN3_SAMPLER_TOP_K:-}" ]]; then
    cli_args+=(--sampler-top-k "$SIM_QWEN3_SAMPLER_TOP_K")
  fi
  if [[ -n "${SIM_QWEN3_SAMPLER_TOP_P_MILLI:-}" ]]; then
    cli_args+=(--sampler-top-p-milli "$SIM_QWEN3_SAMPLER_TOP_P_MILLI")
  fi
  if [[ -n "${SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI:-}" ]]; then
    cli_args+=(--sampler-temperature-milli "$SIM_QWEN3_SAMPLER_TEMPERATURE_MILLI")
  fi
  if [[ -n "${SIM_QWEN3_SAMPLER_SEED:-}" ]]; then
    cli_args+=(--sampler-seed "$SIM_QWEN3_SAMPLER_SEED")
  fi

  if (( memory_decision_reuse )) &&
     (( ! memory_validate_only )) &&
     (( memory_prefix_cache_lookup )) &&
     [[ -z "$SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR" ]]; then
    if ! launch_prefix_cache_service; then
      echo "failed to launch prefix-cache service" >&2
      exit 1
    fi
  fi
  if (( memory_decision_reuse )) && [[ -n "$SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR" ]]; then
    cli_args+=(--memory-prefix-cache-service-addr "$SIM_W5_MEMORY_PREFIX_CACHE_SERVICE_ADDR")
  fi
  if [[ "$SIM_W5_SERVING_QUEUE" == "1" ]]; then
    echo "[w5_inference_cluster] serving_queue=1 launch_mode=ready_only" >&2
    exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
  fi
  echo "[w5_inference_cluster] runtime_boundary_lookup=$memory_runtime_lookup online_boundary_lookup=$memory_online_lookup observation_store=$SIM_W5_MEMORY_OBSERVATION_STORE decision_reuse=$memory_decision_reuse decision_store=$SIM_W5_MEMORY_DECISION_STORE" >&2
  "${SIM_CLI_BIN}" "${cli_args[@]}"
  run_rc=$?
  stop_prefix_cache_service
  exit "$run_rc"
fi

case "$SIM_W5_VALIDATE_ONLY" in
  1|true|TRUE|yes|YES)
    echo "[w5_inference_cluster] config validation passed: no Memory Service runtime path selected" >&2
    exit 0
    ;;
esac

exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
