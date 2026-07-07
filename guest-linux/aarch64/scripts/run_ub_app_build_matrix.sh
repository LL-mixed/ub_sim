#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

APP_NAMES=(
  ub_chat
  ub_rpc
  ub_tcp_each_server
  ub_udma
  ub_obmm_pool
  obmm_queue
  obmm_dataplane_microbench
  obmm_import_stress
  obmm_gsva
  obmm_coh_test
  gva_direct
  gva_manager
  gsva_query
  gsva_coh_test
  gsva_lifecycle_test
  npu_test
  npu_gsva_test
  ssd_test
  ssd_gsva_test
  mem_service
  llm_infer
  serving_control
  pretraining_client
)

usage() {
  cat <<'EOF'
Usage: run_ub_app_build_matrix.sh [--dry-run] [--from APP] [--only APP] [--continue-on-fail] [--keep-artifacts] [--list]

Builds every app-local Makefile under guest-linux/aarch64/apps.
By default, each successful build is followed by make clean so validation does
not leave app binaries in the source tree.

Options:
  --dry-run            Print make commands without executing them.
  --from APP           Skip entries before APP.
  --only APP           Build only one app entry.
  --continue-on-fail   Continue after failures and report them at the end.
  --keep-artifacts     Do not run make clean after successful builds.
  --list               Print known app entries and exit.
  -h, --help           Show this help.
EOF
}

print_entries() {
  local app

  for app in "${APP_NAMES[@]}"; do
    printf '%s makefile=apps/%s/Makefile\n' "$app" "$app"
  done
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

app_exists() {
  local wanted="$1"
  local app

  for app in "${APP_NAMES[@]}"; do
    [[ "$app" == "$wanted" ]] && return 0
  done
  return 1
}

run_make_step() {
  local app="$1"
  local app_dir="$ROOT_DIR/apps/$app"
  local rc=0
  local clean_rc=0

  if [[ ! -f "$app_dir/Makefile" ]]; then
    echo "[app-build-matrix] FAIL: missing Makefile for $app: apps/$app/Makefile" >&2
    return 127
  fi

  printf '[app-build-matrix] RUN app=%s cmd=make -C apps/%s\n' "$app" "$app"
  if [[ "$DRY_RUN" == "1" ]]; then
    if [[ "$KEEP_ARTIFACTS" != "1" ]]; then
      printf '[app-build-matrix] RUN app=%s cmd=make -C apps/%s clean\n' "$app" "$app"
    fi
    return 0
  fi

  make -C "$app_dir" || rc=$?
  if [[ "$KEEP_ARTIFACTS" != "1" ]]; then
    make -C "$app_dir" clean || clean_rc=$?
  fi
  if [[ "$rc" -ne 0 ]]; then
    return "$rc"
  fi
  return "$clean_rc"
}

DRY_RUN=0
FROM_APP=""
FROM_APP_SEEN=0
ONLY_APP=""
CONTINUE_ON_FAIL=0
KEEP_ARTIFACTS=0

while (( $# > 0 )); do
  case "$1" in
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
    --keep-artifacts)
      KEEP_ARTIFACTS=1
      shift
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

if [[ -n "$ONLY_APP" ]] && ! app_exists "$ONLY_APP"; then
  echo "[app-build-matrix] FAIL: --only app not found: $ONLY_APP" >&2
  exit 2
fi
if [[ -n "$FROM_APP" ]] && ! app_exists "$FROM_APP"; then
  echo "[app-build-matrix] FAIL: --from app not found: $FROM_APP" >&2
  exit 2
fi

failures=0
rc=0
for app in "${APP_NAMES[@]}"; do
  if ! should_include_app "$app"; then
    continue
  fi
  run_make_step "$app" || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    failures=$((failures + 1))
    printf '[app-build-matrix] FAIL app=%s rc=%d\n' "$app" "$rc" >&2
    if [[ "$CONTINUE_ON_FAIL" != "1" ]]; then
      exit "$rc"
    fi
    rc=0
  fi
done

if [[ "$failures" -ne 0 ]]; then
  echo "[app-build-matrix] FAIL: $failures app build(s) failed" >&2
  exit 1
fi
printf '[app-build-matrix] PASS\n'
