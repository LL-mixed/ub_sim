#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_APP_DIR="$ROOT_DIR/apps/mem_service"
APP_DIR=""
HOST_BIN=""
BUNDLE_FILE=""
WORK_DIR=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_ops_certification_bundle.sh --bundle-file PATH [--app-dir DIR] [--work-dir DIR] [--dry-run]

Verifies a mem_service real-Linux ops certification bundle artifact.

Options:
  --bundle-file PATH  Bundle produced by run_mem_service_linux_ops_ci.sh.
  --app-dir DIR       mem_service app directory containing linqu_mem_service_host.
                      By default the script first tries the installed libexec
                      binary next to the installed share tree, then falls back
                      to the source-tree app directory.
  --work-dir DIR      Directory used to extract the bundle.
  --dry-run           Print the verification commands without running them.
  -h, --help          Show this help.
EOF
}

resolve_host_bin() {
  local installed_host

  if [[ -n "$APP_DIR" ]]; then
    HOST_BIN="$APP_DIR/linqu_mem_service_host"
    if [[ ! -x "$HOST_BIN" ]]; then
      make -C "$APP_DIR" linqu_mem_service_host
    fi
    return
  fi

  installed_host="$(cd "$SCRIPT_DIR/../../../.." && pwd)/libexec/lingqu/mem_service/linqu_mem_service_host"
  if [[ -x "$installed_host" ]]; then
    HOST_BIN="$installed_host"
    return
  fi

  APP_DIR="$DEFAULT_APP_DIR"
  HOST_BIN="$APP_DIR/linqu_mem_service_host"
  if [[ ! -d "$APP_DIR" ]]; then
    echo "[mem-service-ops-certification-bundle] FAIL: app directory not found: $APP_DIR" >&2
    exit 1
  fi
  if [[ ! -x "$HOST_BIN" ]]; then
    make -C "$APP_DIR" linqu_mem_service_host
  fi
}

print_host_verify_command() {
  local installed_host

  if [[ -n "$APP_DIR" ]]; then
    printf '%s/linqu_mem_service_host ops-certification-verify --evidence-file %s/ops-certification-linux-ci.evidence\n' "$APP_DIR" "$WORK_DIR"
    return
  fi

  installed_host="$(cd "$SCRIPT_DIR/../../../.." && pwd)/libexec/lingqu/mem_service/linqu_mem_service_host"
  if [[ -x "$installed_host" ]]; then
    printf '%s ops-certification-verify --evidence-file %s/ops-certification-linux-ci.evidence\n' "$installed_host" "$WORK_DIR"
    return
  fi

  printf '%s/linqu_mem_service_host ops-certification-verify --evidence-file %s/ops-certification-linux-ci.evidence\n' "$DEFAULT_APP_DIR" "$WORK_DIR"
}

while (( $# > 0 )); do
  case "$1" in
    --bundle-file)
      if (( $# < 2 )); then
        echo "--bundle-file requires a path" >&2
        exit 2
      fi
      BUNDLE_FILE="$2"
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

if [[ -z "$BUNDLE_FILE" ]]; then
  echo "[mem-service-ops-certification-bundle] FAIL: --bundle-file is required" >&2
  exit 2
fi

if [[ -z "$WORK_DIR" ]]; then
  WORK_DIR="/tmp/linqu-mem-service-ops-certification-bundle.verify"
fi

if [[ "$DRY_RUN" == "1" ]]; then
  printf 'tar -tf %s\n' "$BUNDLE_FILE"
  printf 'mkdir -p %s\n' "$WORK_DIR"
  printf 'tar -C %s -xf %s\n' "$WORK_DIR" "$BUNDLE_FILE"
  printf 'test -f %s/ops-certification-bundle.manifest\n' "$WORK_DIR"
  printf 'test -f %s/ops-certification-linux-ci.evidence\n' "$WORK_DIR"
  printf 'test -f %s/ops-certification-upgrade-rollback.marker\n' "$WORK_DIR"
  print_host_verify_command
  exit 0
fi

if [[ ! -f "$BUNDLE_FILE" ]]; then
  echo "[mem-service-ops-certification-bundle] FAIL: bundle file not found: $BUNDLE_FILE" >&2
  exit 1
fi

if ! tar -tf "$BUNDLE_FILE" | grep -q '^./ops-certification-bundle.manifest$'; then
  echo "[mem-service-ops-certification-bundle] FAIL: missing bundle manifest" >&2
  exit 1
fi

while IFS= read -r entry; do
  case "$entry" in
    /*|../*|*/../*)
      echo "[mem-service-ops-certification-bundle] FAIL: unsafe tar entry: $entry" >&2
      exit 1
      ;;
  esac
done < <(tar -tf "$BUNDLE_FILE")

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
tar -C "$WORK_DIR" -xf "$BUNDLE_FILE"

MANIFEST="$WORK_DIR/ops-certification-bundle.manifest"
EVIDENCE="$WORK_DIR/ops-certification-linux-ci.evidence"
MARKER="$WORK_DIR/ops-certification-upgrade-rollback.marker"

test -f "$MANIFEST"
test -f "$EVIDENCE"
test -f "$MARKER"
test -f "$WORK_DIR/release-manifest.txt"
test -f "$WORK_DIR/package-manifest.txt"
test -f "$WORK_DIR/ops-certification-policy.txt"

grep -q '^bundle_schema=linqu-mem-service-ops-certification-bundle-v1$' "$MANIFEST"
grep -q '^bundle_gate=linux-ops-certification-bundle$' "$MANIFEST"
grep -q '^evidence_verify_gate=linux-ops-evidence-verify$' "$MANIFEST"
grep -q '^evidence=ops-certification-linux-ci.evidence$' "$MANIFEST"
grep -q '^upgrade_rollback_marker=ops-certification-upgrade-rollback.marker$' "$MANIFEST"
grep -q '^release_manifest=release-manifest.txt$' "$MANIFEST"
grep -q '^package_manifest=package-manifest.txt$' "$MANIFEST"
grep -q '^ops_certification_policy=ops-certification-policy.txt$' "$MANIFEST"
grep -q '^rpm=linqu-mem-service-.*[.]rpm$' "$MANIFEST"

resolve_host_bin
"$HOST_BIN" ops-certification-verify --evidence-file "$EVIDENCE"
printf '[mem-service-ops-certification-bundle] PASS bundle=%s evidence=%s\n' "$BUNDLE_FILE" "$EVIDENCE"
