#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
BUNDLE_FILE=""
WORK_DIR=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_remote_transport_bundle.sh --bundle-file PATH [--app-dir DIR] [--work-dir DIR] [--dry-run]

Verifies a mem_service production remote transport evidence bundle artifact.

Options:
  --bundle-file PATH  Bundle produced by remote-transport-certification-bundle.
  --app-dir DIR       mem_service app directory containing linqu_mem_service_host.
  --work-dir DIR      Directory used to extract the bundle.
  --dry-run           Print the verification commands without running them.
  -h, --help          Show this help.
EOF
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
  echo "[mem-service-remote-transport-bundle] FAIL: --bundle-file is required" >&2
  exit 2
fi

if [[ -z "$WORK_DIR" ]]; then
  WORK_DIR="/tmp/linqu-mem-service-remote-transport-bundle.verify"
fi

if [[ "$DRY_RUN" == "1" ]]; then
  printf 'tar -tf %s\n' "$BUNDLE_FILE"
  printf 'mkdir -p %s\n' "$WORK_DIR"
  printf 'tar -C %s -xf %s\n' "$WORK_DIR" "$BUNDLE_FILE"
  printf 'test -f %s/remote-transport-bundle.manifest\n' "$WORK_DIR"
  printf 'test -f %s/remote-transport.evidence\n' "$WORK_DIR"
  printf '%s/linqu_mem_service_host remote-transport-verify --evidence-file %s/remote-transport.evidence\n' "$APP_DIR" "$WORK_DIR"
  exit 0
fi

if [[ ! -f "$BUNDLE_FILE" ]]; then
  echo "[mem-service-remote-transport-bundle] FAIL: bundle file not found: $BUNDLE_FILE" >&2
  exit 1
fi

if [[ ! -d "$APP_DIR" ]]; then
  echo "[mem-service-remote-transport-bundle] FAIL: app directory not found: $APP_DIR" >&2
  exit 1
fi

if ! tar -tf "$BUNDLE_FILE" | grep -q '^./remote-transport-bundle.manifest$'; then
  echo "[mem-service-remote-transport-bundle] FAIL: missing bundle manifest" >&2
  exit 1
fi

while IFS= read -r entry; do
  case "$entry" in
    /*|../*|*/../*)
      echo "[mem-service-remote-transport-bundle] FAIL: unsafe tar entry: $entry" >&2
      exit 1
      ;;
  esac
done < <(tar -tf "$BUNDLE_FILE")

rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR"
tar -C "$WORK_DIR" -xf "$BUNDLE_FILE"

MANIFEST="$WORK_DIR/remote-transport-bundle.manifest"
EVIDENCE="$WORK_DIR/remote-transport.evidence"

test -f "$MANIFEST"
test -f "$EVIDENCE"
test -f "$WORK_DIR/release-manifest.txt"
test -f "$WORK_DIR/package-manifest.txt"

grep -q '^bundle_schema=linqu-mem-service-remote-transport-bundle-v1$' "$MANIFEST"
grep -q '^bundle_gate=remote-transport-certification-bundle$' "$MANIFEST"
grep -q '^evidence_verify_gate=remote-transport-evidence-verify$' "$MANIFEST"
grep -q '^evidence=remote-transport.evidence$' "$MANIFEST"
grep -q '^release_manifest=release-manifest.txt$' "$MANIFEST"
grep -q '^package_manifest=package-manifest.txt$' "$MANIFEST"

if [[ ! -x "$APP_DIR/linqu_mem_service_host" ]]; then
  make -C "$APP_DIR" linqu_mem_service_host
fi

"$APP_DIR/linqu_mem_service_host" remote-transport-verify --evidence-file "$EVIDENCE"
printf '[mem-service-remote-transport-bundle] PASS bundle=%s evidence=%s\n' "$BUNDLE_FILE" "$EVIDENCE"
