#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
MATRIX_OUT_DIR="${MATRIX_OUT_DIR:-$OUT_DIR/acceptance_matrix}"
BASE_APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1 linqu_bizmsg_verify=1 linqu_urma_dp_verify=1}"
SUMMARY_FILE="${SUMMARY_FILE:-$MATRIX_OUT_DIR/summary.txt}"
MATRIX_CASES="${MATRIX_CASES:-all}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_matrix_${RANDOM}}"

mkdir -p "$MATRIX_OUT_DIR"
: > "$SUMMARY_FILE"

run_case() {
  local name="$1"
  local iterations="$2"
  local run_secs="$3"
  local start_gap_secs="$4"
  local bench_pkts="$5"
  local bench_interval_us="$6"
  local bench_wait_ms="$7"
  local min_rx_pps="$8"
  local max_loss_ppm="$9"
  local min_pass_rate="${10}"
  local case_log="$MATRIX_OUT_DIR/${name}.log"
  local case_report="$MATRIX_OUT_DIR/${name}.report.txt"
  local case_run_id="${RUN_ID_BASE}_${name}"
  local rc=0

  echo "[matrix] case=${name} iter=${iterations} run_secs=${run_secs} bench_pkts=${bench_pkts} min_rx_pps=${min_rx_pps} max_loss_ppm=${max_loss_ppm}"

  if APPEND_EXTRA="$BASE_APPEND_EXTRA" \
    ITERATIONS="$iterations" \
    RUN_SECS="$run_secs" \
    START_GAP_SECS="$start_gap_secs" \
    BENCH_PKTS="$bench_pkts" \
    BENCH_INTERVAL_US="$bench_interval_us" \
    BENCH_WAIT_MS="$bench_wait_ms" \
    BENCH_MIN_RX_PPS="$min_rx_pps" \
    BENCH_MIN_RX_PPS_GATE="$min_rx_pps" \
    BENCH_MAX_LOSS_PPM="$max_loss_ppm" \
    BENCH_MAX_LOSS_PPM_GATE="$max_loss_ppm" \
    MIN_PASS_RATE_PERCENT="$min_pass_rate" \
    RUN_ID="$case_run_id" \
    REPORT_FILE="$case_report" \
    "$SCRIPT_DIR/run_ub_dual_node_urma_dataplane_workload_test.sh" >"$case_log" 2>&1; then
    rc=0
  else
    rc=$?
  fi

  {
    echo "case=${name}"
    echo "rc=${rc}"
    echo "log=${case_log}"
    echo "report=${case_report}"
    echo "run_id=${case_run_id}"
    if [[ -f "$case_report" ]]; then
      rg -n "^passed=|^failed=|^pass_rate_percent=|^iteration_.*_nodeA_bench=|^iteration_.*_nodeB_bench=" "$case_report" || true
    fi
    echo "---"
  } >> "$SUMMARY_FILE"

  return "$rc"
}

case_enabled() {
  local name="$1"
  if [[ "$MATRIX_CASES" == "all" ]]; then
    return 0
  fi
  [[ ",$MATRIX_CASES," == *",$name,"* ]]
}

failed=0

if case_enabled "smoke_multi_round"; then
  run_case "smoke_multi_round" 3 150 1 20 1000 5000 5 400000 100 || failed=$((failed + 1))
fi
if case_enabled "soak_long_duration"; then
  run_case "soak_long_duration" 4 220 1 30 800 6000 5 400000 100 || failed=$((failed + 1))
fi
if case_enabled "stress_throughput"; then
  run_case "stress_throughput" 3 180 1 40 500 8000 8 450000 100 || failed=$((failed + 1))
fi
if case_enabled "restart_recovery"; then
  run_case "restart_recovery" 6 120 1 20 1200 5000 0 1000000 100 || failed=$((failed + 1))
fi

echo "[matrix] summary: $SUMMARY_FILE"
cat "$SUMMARY_FILE"

if (( failed > 0 )); then
  echo "[matrix] FAILED cases=$failed" >&2
  exit 1
fi

echo "[matrix] all cases passed"
exit 0
