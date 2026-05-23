#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OUT_DIR="$ROOT_DIR/out"

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
    ;;
  *)
    SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-0}"
    ;;
esac

RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE}_${RANDOM}}"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w5_inference_cluster.trace.latest.txt}"
RUN_SUMMARY_FILE="${RUN_SUMMARY_FILE:-$OUT_DIR/eight_node_w5_inference_cluster_summary.${RUN_ID}.txt}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense}"
SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP="${SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP:-0}"
SIM_W5_MEMORY_OBSERVATION_STORE="${SIM_W5_MEMORY_OBSERVATION_STORE:-}"

export RUN_ID
export TRACE_FILE
export RUN_SUMMARY_FILE
export SIM_UAPI_W5_PROFILE
export SIM_UAPI_W4_CHIPBACKEND_PROFILE
export SIM_QWEN3_GUEST_ENGRAM
export SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP
export SIM_W5_MEMORY_OBSERVATION_STORE

if [[ "$SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP" == "1" ||
      "$SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP" == "true" ]]; then
  SIM_CLI_BIN="${SIM_CLI_BIN:-$REPO_DIR/target/debug/sim-cli}"
  if [[ ! -x "$SIM_CLI_BIN" ]]; then
    echo "SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP requires sim-cli: $SIM_CLI_BIN" >&2
    echo "hint: run cargo build -p sim-cli, or set SIM_CLI_BIN to a built sim-cli" >&2
    exit 2
  fi
  if [[ -z "${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}" ]]; then
    echo "SIM_W5_MEMORY_RUNTIME_BOUNDARY_LOOKUP requires SIM_QWEN3_DENSE_WEIGHTS_PATH" >&2
    exit 2
  fi
  if [[ -z "$SIM_W5_MEMORY_OBSERVATION_STORE" ]]; then
    SIM_W5_MEMORY_OBSERVATION_STORE="$OUT_DIR/w5_memory_runtime_boundary_lookup.${RUN_ID}.json"
    export SIM_W5_MEMORY_OBSERVATION_STORE
  fi
  cli_args=(
    w5-inference-cluster
    --script "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
    --w5-profile "$SIM_UAPI_W5_PROFILE"
    --steps "${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
    --weights-path "$SIM_QWEN3_DENSE_WEIGHTS_PATH"
    --prompt-token-ids "${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-81378,37585,374}"
    --memory-observation-store "$SIM_W5_MEMORY_OBSERVATION_STORE"
    --memory-runtime-boundary-lookup
  )
  if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
    cli_args+=(--engram)
  fi
  echo "[w5_inference_cluster] runtime_boundary_lookup=1 store=$SIM_W5_MEMORY_OBSERVATION_STORE" >&2
  exec "$SIM_CLI_BIN" "${cli_args[@]}"
fi

exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
