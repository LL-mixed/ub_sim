#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/mem_service/linux_ops_ci}"
ROLLBACK_RPM=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_linux_ops_ci.sh --rollback-rpm PATH [--out-dir DIR] [--dry-run]

Runs the mem_service real-Linux deployment certification wrapper.

Requirements:
  - Linux host with systemd
  - root privileges
  - rpmbuild, rpm2cpio, cpio, rpm, curl, promtool
  - readable previous-release rollback rpm

Outputs:
  - ops-certification-upgrade-rollback.marker
  - ops-certification-linux-ci.evidence

Options:
  --rollback-rpm PATH  Previous-release rpm used for rollback validation.
  --out-dir DIR        Package/evidence output directory.
  --dry-run            Print the make command without running it.
  -h, --help           Show this help.
EOF
}

while (( $# > 0 )); do
  case "$1" in
    --rollback-rpm)
      if (( $# < 2 )); then
        echo "--rollback-rpm requires a path" >&2
        exit 2
      fi
      ROLLBACK_RPM="$2"
      shift 2
      ;;
    --out-dir)
      if (( $# < 2 )); then
        echo "--out-dir requires a path" >&2
        exit 2
      fi
      OUT_DIR="$2"
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

if [[ -z "$ROLLBACK_RPM" ]]; then
  echo "[mem-service-linux-ops-ci] FAIL: --rollback-rpm is required" >&2
  exit 2
fi

printf '[mem-service-linux-ops-ci] RUN app=mem_service target=linux-ops-deployment-smoke out=%s rollback_rpm=%s\n' "$OUT_DIR" "$ROLLBACK_RPM"
if [[ "$DRY_RUN" == "1" ]]; then
  printf 'make -C %s PACKAGE_OUT_DIR=%s OPS_CERTIFICATION_ROLLBACK_RPM=%s linux-ops-deployment-smoke\n' "$APP_DIR" "$OUT_DIR" "$ROLLBACK_RPM"
  exit 0
fi

make -C "$APP_DIR" \
  "PACKAGE_OUT_DIR=$OUT_DIR" \
  "OPS_CERTIFICATION_ROLLBACK_RPM=$ROLLBACK_RPM" \
  linux-ops-deployment-smoke

printf '[mem-service-linux-ops-ci] PASS evidence=%s marker=%s\n' \
  "$OUT_DIR/ops-certification-linux-ci.evidence" \
  "$OUT_DIR/ops-certification-upgrade-rollback.marker"
