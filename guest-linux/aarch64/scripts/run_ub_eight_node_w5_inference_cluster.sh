#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
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

export RUN_ID
export TRACE_FILE
export RUN_SUMMARY_FILE
export SIM_UAPI_W5_PROFILE
export SIM_UAPI_W4_CHIPBACKEND_PROFILE
export SIM_QWEN3_GUEST_ENGRAM

exec "$SCRIPT_DIR/run_ub_eight_node_w4_guest.sh"
