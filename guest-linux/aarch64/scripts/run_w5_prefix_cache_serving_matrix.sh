#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_PATH="$ROOT_DIR/out/w5_cluster_qwen3_0_6b_2step.matrix.env"
STEPS=8
SAME_PREFIX_RUNS=2
DRY_RUN=0
INCLUDE_EVICTION=1
SHARED_PREFIX_TOKEN_IDS="81378,37585,374"
SUFFIX_A_TOKEN_IDS="13"
SUFFIX_B_TOKEN_IDS="14"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_prefix_cache_serving_matrix.sh [--steps N] [--same-prefix-runs N] [--dry-run]
                                               [--shared-prefix-token-ids CSV]
                                               [--suffix-a-token-ids CSV]
                                               [--suffix-b-token-ids CSV]
                                               [--skip-eviction] [config.env]

Runs a W5 prefix-cache serving matrix:
  1. seed the shared prefix, GSVA KV writeback, memory reuse disabled
  2. one or more request A runs with shared prefix plus suffix A that must hit the prefix cache
  3. request B with the same shared prefix plus divergent suffix B that must hit the same prefix
  4. an evicted/empty reuse-store request A that must fail closed when prefix cache is required

The current W5 guest runtime supports partial-prefix reuse for a single suffix token. Multi-token
suffix prefill is intentionally rejected until W5 can replay every suffix token over the prefix KV.

Defaults: --steps 8 --same-prefix-runs 2 --suffix-a-token-ids 13 --suffix-b-token-ids 14.
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --steps)
      if (( $# < 2 )); then
        echo "--steps requires a value" >&2
        usage
        exit 2
      fi
      STEPS="$2"
      shift 2
      ;;
    --steps=*)
      STEPS="${1#--steps=}"
      shift
      ;;
    --same-prefix-runs)
      if (( $# < 2 )); then
        echo "--same-prefix-runs requires a value" >&2
        usage
        exit 2
      fi
      SAME_PREFIX_RUNS="$2"
      shift 2
      ;;
    --same-prefix-runs=*)
      SAME_PREFIX_RUNS="${1#--same-prefix-runs=}"
      shift
      ;;
    --shared-prefix-token-ids)
      if (( $# < 2 )); then
        echo "--shared-prefix-token-ids requires a value" >&2
        usage
        exit 2
      fi
      SHARED_PREFIX_TOKEN_IDS="$2"
      shift 2
      ;;
    --shared-prefix-token-ids=*)
      SHARED_PREFIX_TOKEN_IDS="${1#--shared-prefix-token-ids=}"
      shift
      ;;
    --suffix-a-token-ids)
      if (( $# < 2 )); then
        echo "--suffix-a-token-ids requires a value" >&2
        usage
        exit 2
      fi
      SUFFIX_A_TOKEN_IDS="$2"
      shift 2
      ;;
    --suffix-a-token-ids=*)
      SUFFIX_A_TOKEN_IDS="${1#--suffix-a-token-ids=}"
      shift
      ;;
    --suffix-b-token-ids)
      if (( $# < 2 )); then
        echo "--suffix-b-token-ids requires a value" >&2
        usage
        exit 2
      fi
      SUFFIX_B_TOKEN_IDS="$2"
      shift 2
      ;;
    --suffix-b-token-ids=*)
      SUFFIX_B_TOKEN_IDS="${1#--suffix-b-token-ids=}"
      shift
      ;;
    --skip-eviction)
      INCLUDE_EVICTION=0
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
      if [[ -n "${CONFIG_ARG_SEEN:-}" ]]; then
        echo "only one config file may be provided" >&2
        usage
        exit 2
      fi
      CONFIG_ARG_SEEN=1
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
if [[ ! "$STEPS" =~ '^[0-9]+$' || "$STEPS" == "0" ]]; then
  echo "--steps must be a positive integer: $STEPS" >&2
  exit 2
fi
if [[ ! "$SAME_PREFIX_RUNS" =~ '^[0-9]+$' || "$SAME_PREFIX_RUNS" == "0" ]]; then
  echo "--same-prefix-runs must be a positive integer: $SAME_PREFIX_RUNS" >&2
  exit 2
fi
if [[ ! "$SHARED_PREFIX_TOKEN_IDS" =~ '^[0-9]+(,[0-9]+)*$' ]]; then
  echo "--shared-prefix-token-ids must be a non-empty token CSV: $SHARED_PREFIX_TOKEN_IDS" >&2
  exit 2
fi
for token_csv in "$SUFFIX_A_TOKEN_IDS" "$SUFFIX_B_TOKEN_IDS"; do
  if [[ -n "$token_csv" && ! "$token_csv" =~ '^[0-9]+(,[0-9]+)*$' ]]; then
    echo "suffix token ids must be empty or token CSV: $token_csv" >&2
    exit 2
  fi
done
if [[ ! "$SUFFIX_A_TOKEN_IDS" =~ '^[0-9]+$' || ! "$SUFFIX_B_TOKEN_IDS" =~ '^[0-9]+$' ]]; then
  echo "W5 partial prefix-cache serving matrix currently requires single-token suffix A/B" >&2
  exit 2
fi
if [[ "$SUFFIX_A_TOKEN_IDS" == "$SUFFIX_B_TOKEN_IDS" ]]; then
  echo "--suffix-a-token-ids and --suffix-b-token-ids must differ for divergent suffix coverage" >&2
  exit 2
fi
if [[ ! -f "$CONFIG_PATH" ]]; then
  echo "W5 cluster config file is missing: $CONFIG_PATH" >&2
  exit 2
fi

set -a
source "$CONFIG_PATH"
set +a

PROFILE="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
OUT_DIR="$ROOT_DIR/out"
RUNNER="$SCRIPT_DIR/run_w5_cluster_config.sh"
REPORT="$SCRIPT_DIR/w5_inference_run_report.py"
SUMMARY_GLOB="eight_node_w5_inference_cluster_summary.*_w5_${PROFILE}_*.txt"
MATRIX_TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/w5_prefix_cache_serving_matrix.XXXXXX")"
EMPTY_REUSE_OUT_DIR="$MATRIX_TMP_DIR/evicted-out"
mkdir -p "$EMPTY_REUSE_OUT_DIR"

cleanup() {
  rm -rf "$MATRIX_TMP_DIR"
}
trap cleanup EXIT

join_prompt_tokens() {
  local prefix="$1"
  local suffix="$2"
  if [[ -z "$suffix" ]]; then
    echo "$prefix"
  else
    echo "$prefix,$suffix"
  fi
}

token_csv_count() {
  local token_csv="$1"
  local -a tokens
  tokens=("${(@s:,:)token_csv}")
  echo "${#tokens[@]}"
}

write_case_config() {
  local label="$1"
  local prompt_tokens="$2"
  local reuse_out_dir="${3:-$OUT_DIR}"
  local decision_run_id="${4:-}"
  local shortpath_execute="${5:-}"
  local include_boundary_selector="${6:-1}"
  local case_config="$MATRIX_TMP_DIR/$label.env"
  cp "$CONFIG_PATH" "$case_config"
  {
    printf '\n'
    printf 'SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS=%s\n' "$prompt_tokens"
    printf 'SIM_W5_MEMORY_REUSE_OUT_DIR=%s\n' "$reuse_out_dir"
    if [[ -n "$shortpath_execute" ]]; then
      printf 'SIM_W5_MEMORY_SHORTPATH_EXECUTE=%s\n' "$shortpath_execute"
    fi
    if [[ -n "$decision_run_id" ]]; then
      printf 'SIM_W5_MEMORY_DECISION_STORE=%s\n' "$OUT_DIR/w5_memory_runtime_boundary_lookup.$decision_run_id.json"
      printf 'SIM_W5_MEMORY_DECISION_OBJECT_STORE=%s\n' "$OUT_DIR/w5_object_service_store.$decision_run_id.json"
      if [[ "$include_boundary_selector" != "0" ]]; then
        printf 'SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID=%s\n' "$decision_run_id"
      fi
    fi
  } >> "$case_config"
  echo "$case_config"
}

summary_run_id() {
  local summary_path="$1"
  local base="${summary_path:t}"
  local run_id="${base#eight_node_w5_inference_cluster_summary.}"
  echo "${run_id%.txt}"
}

latest_summary() {
  local -a summaries
  summaries=("$OUT_DIR"/$~SUMMARY_GLOB(N.om[1]))
  if (( ${#summaries[@]} == 0 )); then
    echo "missing W5 summary for profile=$PROFILE in $OUT_DIR" >&2
    return 1
  fi
  echo "${summaries[1]}"
}

run_case() {
  local label="$1"
  local case_config="$2"
  shift 2
  echo "[w5_prefix_cache_serving_matrix] run=$label steps=$STEPS profile=$PROFILE config=$case_config"
  if (( DRY_RUN )); then
    printf '%q ' "$RUNNER" --steps "$STEPS" --gsva-kv "$@" "$case_config"
    printf '\n'
    return 0
  fi
  "$RUNNER" --steps "$STEPS" --gsva-kv "$@" "$case_config"
}

run_expected_fail_closed() {
  local label="$1"
  local case_config="$2"
  local log_path="$MATRIX_TMP_DIR/$label.stderr"
  echo "[w5_prefix_cache_serving_matrix] run=$label expect_fail_closed=true"
  if (( DRY_RUN )); then
    printf 'expect-fail '
    printf '%q ' "$RUNNER" --steps "$STEPS" --gsva-kv --require-prefix-cache "$case_config"
    printf '\n'
    return 0
  fi
  set +e
  "$RUNNER" --steps "$STEPS" --gsva-kv --require-prefix-cache "$case_config" 2>"$log_path"
  local rc=$?
  set -e
  if (( rc == 0 )); then
    echo "[w5_prefix_cache_serving_matrix] FAIL: $label unexpectedly succeeded" >&2
    return 1
  fi
  if ! grep -q "SIM_W5_REQUIRE_PREFIX_CACHE requires" "$log_path"; then
    echo "[w5_prefix_cache_serving_matrix] FAIL: $label did not fail closed with prefix-cache guard" >&2
    tail -n 40 "$log_path" >&2
    return 1
  fi
  echo "[w5_prefix_cache_serving_matrix] fail_closed=$label status=pass rc=$rc"
}

prompt_a="$(join_prompt_tokens "$SHARED_PREFIX_TOKEN_IDS" "$SUFFIX_A_TOKEN_IDS")"
prompt_b="$(join_prompt_tokens "$SHARED_PREFIX_TOKEN_IDS" "$SUFFIX_B_TOKEN_IDS")"
shared_prefix_token_count="$(token_csv_count "$SHARED_PREFIX_TOKEN_IDS")"
config_seed_prefix="$(write_case_config shared-prefix-seed "$SHARED_PREFIX_TOKEN_IDS")"
config_evicted="$(write_case_config request-a-evicted "$prompt_a" "$EMPTY_REUSE_OUT_DIR")"

echo "=== W5 Prefix Cache Serving Matrix ==="
echo "Profile:          $PROFILE"
echo "Steps:            $STEPS"
echo "Same-prefix runs: $SAME_PREFIX_RUNS"
echo "Shared prefix:    $SHARED_PREFIX_TOKEN_IDS"
echo "Shared tokens:    $shared_prefix_token_count"
echo "Suffix A:         ${SUFFIX_A_TOKEN_IDS:-<empty>}"
echo "Suffix B:         ${SUFFIX_B_TOKEN_IDS:-<empty>}"
echo "Prompt A:         $prompt_a"
echo "Prompt B:         $prompt_b"
echo "Config:           $CONFIG_PATH"

run_case seed-shared-prefix "$config_seed_prefix" --no-memory-reuse
if (( DRY_RUN )); then
  seed_summary="$OUT_DIR/eight_node_w5_inference_cluster_summary.<seed-shared-prefix-run>.txt"
  seed_run_id="<seed-shared-prefix-run>"
else
  seed_summary="$(latest_summary)"
  seed_run_id="$(summary_run_id "$seed_summary")"
fi
echo "[w5_prefix_cache_serving_matrix] seed_summary=$seed_summary"

config_a="$(write_case_config request-a-reuse "$prompt_a" "$OUT_DIR" "$seed_run_id" 0 0)"
config_b="$(write_case_config request-b "$prompt_b" "$OUT_DIR" "$seed_run_id" 0 0)"

first_reuse_summary=""
reuse_index=1
while (( reuse_index <= SAME_PREFIX_RUNS )); do
  run_case "reuse-request-a-$reuse_index" "$config_a" --require-prefix-cache
  if (( DRY_RUN )); then
    reuse_summary="$OUT_DIR/eight_node_w5_inference_cluster_summary.<reuse-request-a-$reuse_index-run>.txt"
    printf '%q ' "$REPORT" "$reuse_summary" --require-prefix-cache --expect-prefix-cache-matched-tokens "$shared_prefix_token_count"
    printf '\n'
  else
    reuse_summary="$(latest_summary)"
    echo "[w5_prefix_cache_serving_matrix] reuse_summary=$reuse_summary"
    "$REPORT" "$reuse_summary" --require-prefix-cache --expect-prefix-cache-matched-tokens "$shared_prefix_token_count"
  fi
  if [[ -z "$first_reuse_summary" ]]; then
    first_reuse_summary="$reuse_summary"
    run_case divergent-suffix-request-b "$config_b" --require-prefix-cache
    if (( DRY_RUN )); then
      mismatch_summary="$OUT_DIR/eight_node_w5_inference_cluster_summary.<divergent-suffix-request-b-run>.txt"
      printf '%q ' "$REPORT" "$mismatch_summary" --require-prefix-cache --expect-prefix-cache-matched-tokens "$shared_prefix_token_count"
      printf '\n'
    else
      mismatch_summary="$(latest_summary)"
      echo "[w5_prefix_cache_serving_matrix] mismatch_summary=$mismatch_summary"
      "$REPORT" "$mismatch_summary" --require-prefix-cache --expect-prefix-cache-matched-tokens "$shared_prefix_token_count"
    fi
  fi
  (( reuse_index += 1 ))
done

if (( INCLUDE_EVICTION )); then
  run_expected_fail_closed evicted-require-prefix-cache "$config_evicted"
fi

echo "[w5_prefix_cache_serving_matrix] PASS"
