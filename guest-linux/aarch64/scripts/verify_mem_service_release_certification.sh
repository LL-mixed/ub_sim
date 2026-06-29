#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR=""
OPS_BUNDLE_FILE=""
REMOTE_TRANSPORT_BUNDLE_FILE=""
WORK_DIR=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_release_certification.sh --ops-bundle-file PATH --remote-transport-bundle-file PATH [--app-dir DIR] [--work-dir DIR] [--dry-run]

Verifies that a mem_service release has independently replayable certification artifacts.

Options:
  --ops-bundle-file PATH                 Linux ops certification bundle.
  --remote-transport-bundle-file PATH    Remote transport certification bundle.
  --app-dir DIR                          Optional mem_service app directory
                                         override for source-tree verification.
                                         By default nested verifiers use the
                                         installed libexec binary next to the
                                         installed share tree, then fall back to
                                         the source-tree app directory.
  --work-dir DIR                         Directory used to extract nested bundles.
  --dry-run                              Print the verification commands without running them.
  -h, --help                             Show this help.
EOF
}

while (( $# > 0 )); do
  case "$1" in
    --ops-bundle-file)
      if (( $# < 2 )); then
        echo "--ops-bundle-file requires a path" >&2
        exit 2
      fi
      OPS_BUNDLE_FILE="$2"
      shift 2
      ;;
    --remote-transport-bundle-file)
      if (( $# < 2 )); then
        echo "--remote-transport-bundle-file requires a path" >&2
        exit 2
      fi
      REMOTE_TRANSPORT_BUNDLE_FILE="$2"
      shift 2
      ;;
    --app-dir)
      if (( $# < 2 )); then
        echo "--app-dir requires a path" >&2
        exit 2
      fi
      APP_DIR="$2"
      shift 2
      ;;
    --work-dir)
      if (( $# < 2 )); then
        echo "--work-dir requires a path" >&2
        exit 2
      fi
      WORK_DIR="$2"
      shift 2
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
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$OPS_BUNDLE_FILE" ]]; then
  echo "[mem-service-release-certification] FAIL: --ops-bundle-file is required" >&2
  exit 2
fi
if [[ -z "$REMOTE_TRANSPORT_BUNDLE_FILE" ]]; then
  echo "[mem-service-release-certification] FAIL: --remote-transport-bundle-file is required" >&2
  exit 2
fi

if [[ -z "$WORK_DIR" ]]; then
  WORK_DIR="/tmp/linqu-mem-service-release-certification.verify"
fi

OPS_VERIFY_WORK_DIR="$WORK_DIR/ops"
REMOTE_TRANSPORT_VERIFY_WORK_DIR="$WORK_DIR/remote-transport"

if [[ "$DRY_RUN" == "1" ]]; then
  if [[ -n "$APP_DIR" ]]; then
    printf '%s/verify_mem_service_ops_certification_bundle.sh --bundle-file %s --app-dir %s --work-dir %s\n' "$SCRIPT_DIR" "$OPS_BUNDLE_FILE" "$APP_DIR" "$OPS_VERIFY_WORK_DIR"
    printf '%s/verify_mem_service_remote_transport_bundle.sh --bundle-file %s --app-dir %s --work-dir %s\n' "$SCRIPT_DIR" "$REMOTE_TRANSPORT_BUNDLE_FILE" "$APP_DIR" "$REMOTE_TRANSPORT_VERIFY_WORK_DIR"
  else
    printf '%s/verify_mem_service_ops_certification_bundle.sh --bundle-file %s --work-dir %s\n' "$SCRIPT_DIR" "$OPS_BUNDLE_FILE" "$OPS_VERIFY_WORK_DIR"
    printf '%s/verify_mem_service_remote_transport_bundle.sh --bundle-file %s --work-dir %s\n' "$SCRIPT_DIR" "$REMOTE_TRANSPORT_BUNDLE_FILE" "$REMOTE_TRANSPORT_VERIFY_WORK_DIR"
  fi
  exit 0
fi

if [[ ! -f "$OPS_BUNDLE_FILE" ]]; then
  echo "[mem-service-release-certification] FAIL: ops bundle file not found: $OPS_BUNDLE_FILE" >&2
  exit 1
fi
if [[ ! -f "$REMOTE_TRANSPORT_BUNDLE_FILE" ]]; then
  echo "[mem-service-release-certification] FAIL: remote transport bundle file not found: $REMOTE_TRANSPORT_BUNDLE_FILE" >&2
  exit 1
fi
mkdir -p "$WORK_DIR"
if [[ -n "$APP_DIR" ]]; then
  "$SCRIPT_DIR/verify_mem_service_ops_certification_bundle.sh" \
    --bundle-file "$OPS_BUNDLE_FILE" \
    --app-dir "$APP_DIR" \
    --work-dir "$OPS_VERIFY_WORK_DIR"
  "$SCRIPT_DIR/verify_mem_service_remote_transport_bundle.sh" \
    --bundle-file "$REMOTE_TRANSPORT_BUNDLE_FILE" \
    --app-dir "$APP_DIR" \
    --work-dir "$REMOTE_TRANSPORT_VERIFY_WORK_DIR"
else
  "$SCRIPT_DIR/verify_mem_service_ops_certification_bundle.sh" \
    --bundle-file "$OPS_BUNDLE_FILE" \
    --work-dir "$OPS_VERIFY_WORK_DIR"
  "$SCRIPT_DIR/verify_mem_service_remote_transport_bundle.sh" \
    --bundle-file "$REMOTE_TRANSPORT_BUNDLE_FILE" \
    --work-dir "$REMOTE_TRANSPORT_VERIFY_WORK_DIR"
fi

printf '[mem-service-release-certification] PASS ops_bundle=%s remote_transport_bundle=%s\n' "$OPS_BUNDLE_FILE" "$REMOTE_TRANSPORT_BUNDLE_FILE"
