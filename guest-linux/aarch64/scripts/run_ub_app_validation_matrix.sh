#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
REPORT_FILE="${REPORT_FILE:-$OUT_DIR/app_validation_matrix.latest.txt}"
STATUS_FILE="${STATUS_FILE:-$OUT_DIR/app_validation_matrix.status.latest.txt}"

APP_ENTRIES=(
  "ub_chat|scripts/run_ub_dual_node_chat.sh|scripts/run_ub_eight_node_chat_matrix.sh"
  "ub_rpc|scripts/run_ub_dual_node_rpc.sh|scripts/run_ub_eight_node_rpc_matrix.sh"
  "ub_tcp_each_server|scripts/run_ub_dual_node_tcp_each_server.sh|scripts/run_ub_eight_node_tcp_each_server_matrix.sh"
  "ub_udma|scripts/run_ub_dual_node_udma.sh|scripts/run_ub_eight_node_udma_matrix.sh"
  "ub_obmm_pool|scripts/run_ub_dual_node_obmm_pool.sh|scripts/run_ub_eight_node_obmm_pool.sh"
  "obmm_queue|scripts/run_ub_dual_node_obmm_queue.sh|scripts/run_ub_eight_node_obmm_queue.sh"
  "obmm_dataplane_microbench|scripts/run_ub_dual_node_obmm_dataplane_microbench_matrix.sh|scripts/run_ub_eight_node_obmm_dataplane_microbench.sh"
  "obmm_import_stress|scripts/run_ub_dual_node_obmm_import_stress.sh|scripts/run_ub_eight_node_obmm_import_stress.sh"
  "obmm_gsva|scripts/run_ub_dual_node_obmm_gsva.sh|scripts/run_ub_eight_node_obmm_gsva_matrix.sh"
  "obmm_coh_test|scripts/run_ub_dual_node_obmm_coh_test.sh|scripts/run_ub_eight_node_obmm_coh_test.sh"
  "gva_direct|scripts/run_ub_dual_node_gva_direct_test.sh|scripts/run_ub_eight_node_gva_direct_test.sh"
  "gva_manager|scripts/run_ub_dual_node_gsva_manager_bootstrap.sh|scripts/run_ub_eight_node_gsva_manager_bootstrap.sh"
  "gsva_query|scripts/run_ub_dual_node_gsva_query.sh|scripts/run_ub_eight_node_gsva_query_caps.sh"
  "gsva_coh_test|scripts/run_ub_two_node_gsva_coh_test.sh|scripts/run_ub_eight_node_gsva_coh_test.sh"
  "gsva_lifecycle_test|scripts/run_ub_two_node_gsva_lifecycle_test.sh|scripts/run_ub_eight_node_gsva_lifecycle_test.sh"
  "npu_test|scripts/run_ub_two_node_npu_test.sh|scripts/run_ub_eight_node_npu_test.sh"
  "npu_gsva_test|scripts/run_ub_two_node_npu_gsva_test.sh|scripts/run_ub_eight_node_npu_gsva_test.sh"
  "ssd_test|scripts/run_ub_two_node_ssd_test.sh|scripts/run_ub_eight_node_ssd_test.sh"
  "ssd_gsva_test|scripts/run_ub_two_node_ssd_gsva_test.sh|scripts/run_ub_eight_node_ssd_gsva_test.sh"
  "mem_service|scripts/run_ub_dual_node_mem_service.sh|scripts/run_ub_eight_node_mem_service.sh"
  "llm_infer|scripts/run_ub_dual_node_w4_guest.sh|scripts/run_ub_eight_node_w4_guest_qwen3_0_6b_2step.sh"
)
W5_ENTRY="w5_inference_cluster|scripts/run_w5_cluster_qwen3_0_6b_2step.sh"

usage() {
  cat <<'EOF'
Usage: run_ub_app_validation_matrix.sh [--scope 2-node|8-node|all|w5|all-with-w5] [--dry-run] [--from APP] [--only APP] [--continue-on-fail] [--resume] [--reset-status]

Runs stable app validation entrypoints from guest-linux/aarch64/apps/README.md.

Options:
  --scope S            Validation scope. Default: 8-node.
  --dry-run            Print commands without executing them.
  --from APP           Skip entries before APP.
  --only APP           Run only one app entry.
  --continue-on-fail   Continue after failures and report them at the end.
  --resume             Skip app/scope pairs already recorded as PASS in the status file.
  --reset-status       Clear the status file before running. Without this, status is cumulative.
  --status-file PATH   Cumulative PASS/FAIL status file. Default: guest-linux/aarch64/out/app_validation_matrix.status.latest.txt.
  --list               Print known app entries and exit.
  -h, --help           Show this help.
EOF
}

split_entry() {
  local entry="$1"
  reply=("${(@s:|:)entry}")
}

print_entries() {
  local entry
  local parts

  for entry in "${APP_ENTRIES[@]}"; do
    split_entry "$entry"
    parts=("${reply[@]}")
    printf '%s 2-node=%s 8-node=%s\n' "$parts[1]" "$parts[2]" "$parts[3]"
  done
  split_entry "$W5_ENTRY"
  parts=("${reply[@]}")
  printf '%s 8-node=%s\n' "$parts[1]" "$parts[2]"
}

script_abs_path() {
  local rel_path="$1"
  echo "$ROOT_DIR/$rel_path"
}

record_status() {
  local app="$1"
  local scope="$2"
  local status_value="$3"
  local rc="$4"
  local rel_path="$5"

  printf '%s|%s|%s|%s|%s\n' "$app" "$scope" "$status_value" "$rc" "$rel_path" >> "$STATUS_FILE"
}

status_has_pass() {
  local app="$1"
  local scope="$2"

  [[ -f "$STATUS_FILE" ]] || return 1
  awk -F'|' -v app="$app" -v scope="$scope" \
    '$1 == app && $2 == scope && $3 == "PASS" { found = 1 } END { exit found ? 0 : 1 }' \
    "$STATUS_FILE"
}

run_step() {
  local app="$1"
  local scope="$2"
  local rel_path="$3"
  local abs_path
  local rc=0

  abs_path="$(script_abs_path "$rel_path")"
  if [[ ! -x "$abs_path" ]]; then
    echo "[app-matrix] FAIL: missing executable for $app $scope: $rel_path" >&2
    return 127
  fi
  if [[ "$RESUME" == "1" ]] && status_has_pass "$app" "$scope"; then
    printf '[app-matrix] SKIP app=%s scope=%s status=PASS cmd=%s\n' "$app" "$scope" "$rel_path" | tee -a "$REPORT_FILE"
    return 0
  fi
  printf '[app-matrix] RUN app=%s scope=%s cmd=%s\n' "$app" "$scope" "$rel_path" | tee -a "$REPORT_FILE"
  if [[ "$DRY_RUN" == "1" ]]; then
    return 0
  fi
  "$abs_path" || rc=$?
  if [[ "$rc" -eq 0 ]]; then
    record_status "$app" "$scope" "PASS" "$rc" "$rel_path"
  else
    record_status "$app" "$scope" "FAIL" "$rc" "$rel_path"
  fi
  return "$rc"
}

should_include_app() {
  local app="$1"

  if [[ -n "$ONLY_APP" && "$app" != "$ONLY_APP" ]]; then
    return 1
  fi
  if [[ -n "$FROM_APP" && "$FROM_APP_SEEN" == "0" ]]; then
    if [[ "$app" == "$FROM_APP" ]]; then
      FROM_APP_SEEN=1
    else
      return 1
    fi
  fi
  return 0
}

run_app_entries() {
  local entry
  local parts
  local app
  local rc=0
  local failures=0

  for entry in "${APP_ENTRIES[@]}"; do
    split_entry "$entry"
    parts=("${reply[@]}")
    app="$parts[1]"
    if ! should_include_app "$app"; then
      continue
    fi
    case "$SCOPE" in
      2-node)
        run_step "$app" "2-node" "$parts[2]" || rc=$?
        ;;
      8-node)
        run_step "$app" "8-node" "$parts[3]" || rc=$?
        ;;
      all|all-with-w5)
        run_step "$app" "2-node" "$parts[2]" || rc=$?
        if [[ "$rc" -eq 0 || "$CONTINUE_ON_FAIL" == "1" ]]; then
          run_step "$app" "8-node" "$parts[3]" || rc=$?
        fi
        ;;
      *)
        echo "[app-matrix] FAIL: unsupported app scope: $SCOPE" >&2
        return 2
        ;;
    esac
    if [[ "$rc" -ne 0 ]]; then
      failures=$((failures + 1))
      printf '[app-matrix] FAIL app=%s rc=%d\n' "$app" "$rc" | tee -a "$REPORT_FILE"
      if [[ "$CONTINUE_ON_FAIL" != "1" ]]; then
        return "$rc"
      fi
      rc=0
    fi
  done

  if [[ -n "$FROM_APP" && "$FROM_APP_SEEN" == "0" ]]; then
    echo "[app-matrix] FAIL: --from app not found: $FROM_APP" >&2
    return 2
  fi
  if [[ -n "$ONLY_APP" && "$failures" -eq 0 ]]; then
    local found=0
    for entry in "${APP_ENTRIES[@]}"; do
      split_entry "$entry"
      parts=("${reply[@]}")
      [[ "$parts[1]" == "$ONLY_APP" ]] && found=1
    done
    if [[ "$found" -eq 0 ]]; then
      echo "[app-matrix] FAIL: --only app not found: $ONLY_APP" >&2
      return 2
    fi
  fi
  [[ "$failures" -eq 0 ]]
}

run_w5_entry() {
  local parts
  split_entry "$W5_ENTRY"
  parts=("${reply[@]}")
  run_step "$parts[1]" "8-node" "$parts[2]"
}

SCOPE="8-node"
DRY_RUN=0
FROM_APP=""
FROM_APP_SEEN=0
ONLY_APP=""
CONTINUE_ON_FAIL=0
RESUME=0
RESET_STATUS=0

while (( $# > 0 )); do
  case "$1" in
    --scope)
      if (( $# < 2 )); then
        echo "--scope requires a value" >&2
        exit 2
      fi
      SCOPE="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --from)
      if (( $# < 2 )); then
        echo "--from requires an app name" >&2
        exit 2
      fi
      FROM_APP="$2"
      shift 2
      ;;
    --only)
      if (( $# < 2 )); then
        echo "--only requires an app name" >&2
        exit 2
      fi
      ONLY_APP="$2"
      shift 2
      ;;
    --continue-on-fail)
      CONTINUE_ON_FAIL=1
      shift
      ;;
    --resume)
      RESUME=1
      shift
      ;;
    --reset-status)
      RESET_STATUS=1
      shift
      ;;
    --status-file)
      if (( $# < 2 )); then
        echo "--status-file requires a path" >&2
        exit 2
      fi
      STATUS_FILE="$2"
      shift 2
      ;;
    --list)
      print_entries
      exit 0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

case "$SCOPE" in
  2-node|8-node|all|w5|all-with-w5)
    ;;
  *)
    echo "unsupported --scope: $SCOPE" >&2
    usage >&2
    exit 2
    ;;
esac

mkdir -p "$OUT_DIR"
mkdir -p "${REPORT_FILE:h}"
mkdir -p "${STATUS_FILE:h}"
: > "$REPORT_FILE"
if [[ "$DRY_RUN" != "1" && "$RESET_STATUS" == "1" ]]; then
  : > "$STATUS_FILE"
fi
if [[ "$DRY_RUN" != "1" ]]; then
  touch "$STATUS_FILE"
fi
printf '[app-matrix] scope=%s dry_run=%s from=%s only=%s continue_on_fail=%s resume=%s reset_status=%s status_file=%s\n' \
  "$SCOPE" "$DRY_RUN" "$FROM_APP" "$ONLY_APP" "$CONTINUE_ON_FAIL" "$RESUME" "$RESET_STATUS" "$STATUS_FILE" | tee -a "$REPORT_FILE"

case "$SCOPE" in
  w5)
    run_w5_entry
    ;;
  all-with-w5)
    app_rc=0
    w5_rc=0
    run_app_entries || app_rc=$?
    if [[ "$app_rc" -eq 0 || "$CONTINUE_ON_FAIL" == "1" ]]; then
      run_w5_entry || w5_rc=$?
    fi
    if [[ "$app_rc" -ne 0 ]]; then
      exit "$app_rc"
    fi
    exit "$w5_rc"
    ;;
  *)
    run_app_entries
    ;;
esac
