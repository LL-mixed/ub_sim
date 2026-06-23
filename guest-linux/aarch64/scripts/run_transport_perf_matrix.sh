#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"

RUN_ID="$(date +%Y-%m-%d_%H-%M-%S)_transport_perf_${RANDOM}"
SIZE=2097152
ITERATIONS=8192
CHUNK_SIZE=64
VERIFY=0
RUN_DATAPLANE=1
RUN_TCP=1
DRY_RUN=0
REPORT_DIR=""
DP_MODES="legacy-pa generic-gva gsva"
TCP_PAIR_LIST_OVERRIDE=""

usage() {
  cat <<'EOF'
Usage: run_transport_perf_matrix.sh [options]

Runs the reusable transport performance matrix for the current W5 transport
stack:
  - OBMM dataplane legacy-pa
  - OBMM dataplane generic-gva
  - OBMM dataplane gsva
  - TCP payload benchmark over the traditional guest network path

Options:
  --run-id ID          Stable run id used in report and log paths.
  --out-dir DIR        Directory for per-case reports and summary files.
  --size BYTES         Payload working-set size. Default: 2097152.
  --iterations N       Benchmark iterations. Default: 8192.
  --chunk-size BYTES   Bytes per benchmark operation. Default: 64.
  --verify             Enable payload verification.
  --no-verify          Disable payload verification. Default.
  --dataplane-only     Run only legacy-pa/generic-gva/gsva dataplane cases.
  --tcp-only           Run only the TCP network baseline case.
  --skip-dataplane     Skip dataplane cases.
  --skip-tcp           Skip TCP baseline.
  --tcp-pair A:B       Restrict TCP benchmark to one pair. Repeatable.
  --quick              Short smoke matrix: smaller payload and nodeA:nodeB TCP.
  --dry-run            Print the commands without launching QEMU.
  -h, --help           Show this help.

Examples:
  guest-linux/aarch64/scripts/run_transport_perf_matrix.sh
  guest-linux/aarch64/scripts/run_transport_perf_matrix.sh --quick
  guest-linux/aarch64/scripts/run_transport_perf_matrix.sh --run-id w5_transport_001 --size 2097152 --iterations 8192
EOF
}

die() {
  echo "run_transport_perf_matrix: error: $*" >&2
  exit 2
}

need_value() {
  local opt="$1"
  local value="${2:-}"
  [[ -n "$value" ]] || die "$opt requires a value"
}

validate_positive_int() {
  local name="$1"
  local value="$2"
  [[ "$value" =~ '^[0-9]+$' ]] || die "$name must be a positive integer: $value"
  (( value > 0 )) || die "$name must be greater than zero: $value"
}

append_tcp_pair() {
  local pair="$1"
  local left="${pair%%:*}"
  local right="${pair##*:}"

  [[ "$pair" == *:* ]] || die "--tcp-pair must use A:B form: $pair"
  [[ -n "$left" && -n "$right" ]] || die "--tcp-pair has empty endpoint: $pair"
  [[ "$left" != "$right" ]] || die "--tcp-pair cannot use the same node twice: $pair"

  if [[ -n "$TCP_PAIR_LIST_OVERRIDE" ]]; then
    TCP_PAIR_LIST_OVERRIDE+=$'\n'
  fi
  TCP_PAIR_LIST_OVERRIDE+="${left} ${right}"
}

quote_command() {
  local arg
  for arg in "$@"; do
    printf ' %q' "$arg"
  done
  printf '\n'
}

run_step() {
  local label="$1"
  shift

  echo "transport_perf_matrix: start case=$label"
  if (( DRY_RUN )); then
    printf 'dry_run: case=%s command=' "$label"
    quote_command "$@"
    return 0
  fi
  "$@"
}

while (( $# > 0 )); do
  case "$1" in
    --run-id)
      need_value "$1" "${2:-}"
      RUN_ID="$2"
      shift 2
      ;;
    --out-dir)
      need_value "$1" "${2:-}"
      REPORT_DIR="$2"
      shift 2
      ;;
    --size)
      need_value "$1" "${2:-}"
      SIZE="$2"
      shift 2
      ;;
    --iterations|--iters)
      need_value "$1" "${2:-}"
      ITERATIONS="$2"
      shift 2
      ;;
    --chunk-size)
      need_value "$1" "${2:-}"
      CHUNK_SIZE="$2"
      shift 2
      ;;
    --verify)
      VERIFY=1
      shift
      ;;
    --no-verify)
      VERIFY=0
      shift
      ;;
    --dataplane-only)
      RUN_DATAPLANE=1
      RUN_TCP=0
      shift
      ;;
    --tcp-only)
      RUN_DATAPLANE=0
      RUN_TCP=1
      shift
      ;;
    --skip-dataplane)
      RUN_DATAPLANE=0
      shift
      ;;
    --skip-tcp)
      RUN_TCP=0
      shift
      ;;
    --tcp-pair)
      need_value "$1" "${2:-}"
      append_tcp_pair "$2"
      shift 2
      ;;
    --quick)
      SIZE=262144
      ITERATIONS=1024
      CHUNK_SIZE=64
      if [[ -z "$TCP_PAIR_LIST_OVERRIDE" ]]; then
        append_tcp_pair "nodeA:nodeB"
      fi
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
    *)
      die "unknown option: $1"
      ;;
  esac
done

validate_positive_int "--size" "$SIZE"
validate_positive_int "--iterations" "$ITERATIONS"
validate_positive_int "--chunk-size" "$CHUNK_SIZE"
[[ "$VERIFY" == "0" || "$VERIFY" == "1" ]] || die "--verify state must be 0 or 1"
(( RUN_DATAPLANE || RUN_TCP )) || die "at least one case must be enabled"

if [[ -z "$REPORT_DIR" ]]; then
  REPORT_DIR="$OUT_DIR/transport_perf.${RUN_ID}"
fi

DP_REPORT="$REPORT_DIR/dataplane.txt"
DP_TRACE="$REPORT_DIR/dataplane.trace.txt"
TCP_REPORT="$REPORT_DIR/tcp.txt"
TCP_TRACE="$REPORT_DIR/tcp.trace.txt"
SUMMARY_TEXT="$REPORT_DIR/summary.txt"
SUMMARY_JSON="$REPORT_DIR/summary.json"

typeset -a reports
reports=()
result="PASS"

echo "transport_perf_matrix: run_id=$RUN_ID"
echo "transport_perf_matrix: report_dir=$REPORT_DIR"
echo "transport_perf_matrix: size=$SIZE iterations=$ITERATIONS chunk_size=$CHUNK_SIZE verify=$VERIFY"
echo "transport_perf_matrix: dataplane=$RUN_DATAPLANE tcp=$RUN_TCP"
if [[ -n "$TCP_PAIR_LIST_OVERRIDE" ]]; then
  echo "transport_perf_matrix: tcp_pairs=custom"
else
  echo "transport_perf_matrix: tcp_pairs=all"
fi

if (( ! DRY_RUN )); then
  mkdir -p "$REPORT_DIR"
fi

if (( RUN_DATAPLANE )); then
  dataplane_cmd=(
    env
    "RUN_ID=${RUN_ID}_dataplane"
    "REPORT_FILE=$DP_REPORT"
    "TRACE_FILE=$DP_TRACE"
    "DP_MODES_OVERRIDE=$DP_MODES"
    "DP_SIZE=$SIZE"
    "DP_ITERS=$ITERATIONS"
    "DP_CHUNK_SIZE=$CHUNK_SIZE"
    "DP_VERIFY=$VERIFY"
    "$SCRIPT_DIR/run_ub_eight_node_obmm_dataplane_microbench.sh"
  )
  if run_step "dataplane" "${dataplane_cmd[@]}"; then
    reports+=("$DP_REPORT")
  else
    result="FAIL"
  fi
fi

if (( RUN_TCP )); then
  tcp_cmd=(
    env
    "RUN_ID=${RUN_ID}_tcp"
    "REPORT_FILE=$TCP_REPORT"
    "TRACE_FILE=$TCP_TRACE"
    "TCP_BENCHMARK=1"
    "TCP_BENCH_SIZE=$SIZE"
    "TCP_BENCH_ITERATIONS=$ITERATIONS"
    "TCP_BENCH_CHUNK_SIZE=$CHUNK_SIZE"
    "TCP_BENCH_VERIFY=$VERIFY"
  )
  if [[ -n "$TCP_PAIR_LIST_OVERRIDE" ]]; then
    tcp_cmd+=("PAIR_LIST_OVERRIDE=$TCP_PAIR_LIST_OVERRIDE")
  fi
  tcp_cmd+=("$SCRIPT_DIR/run_ub_eight_node_tcp_each_server_matrix.sh")

  if run_step "tcp" "${tcp_cmd[@]}"; then
    reports+=("$TCP_REPORT")
  else
    result="FAIL"
  fi
fi

if (( DRY_RUN )); then
  printf 'dry_run: summary_command='
  quote_command "$SCRIPT_DIR/transport_perf_report.py" "${reports[@]}"
  echo "transport_perf_matrix: dry_run=1 result=$result"
  exit 0
fi

if (( ${#reports[@]} == 0 )); then
  echo "transport_perf_matrix: result=FAIL reason=no_case_reports" >&2
  exit 1
fi

if "$SCRIPT_DIR/transport_perf_report.py" "${reports[@]}" | tee "$SUMMARY_TEXT"; then
  "$SCRIPT_DIR/transport_perf_report.py" --json "${reports[@]}" > "$SUMMARY_JSON"
else
  result="FAIL"
fi

echo "transport_perf_matrix: summary=$SUMMARY_TEXT"
echo "transport_perf_matrix: summary_json=$SUMMARY_JSON"
echo "transport_perf_matrix: result=$result"

[[ "$result" == "PASS" ]]
