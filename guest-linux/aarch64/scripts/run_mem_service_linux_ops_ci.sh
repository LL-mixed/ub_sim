#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/mem_service/linux_ops_ci}"
ROLLBACK_RPM=""
DRY_RUN=0
PRE_FLIGHT=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_linux_ops_ci.sh --rollback-rpm PATH [--out-dir DIR] [--preflight] [--dry-run]

Runs the mem_service real-Linux deployment certification wrapper.

Requirements:
  - Linux host with systemd
  - root privileges
  - rpmbuild, rpm2cpio, cpio, rpm, curl, promtool
  - readable previous-release rollback rpm

Outputs:
  - ops-certification-upgrade-rollback.marker
  - ops-certification-linux-ci.evidence
  - linqu-mem-service-ops-certification-bundle.tar

Options:
  --rollback-rpm PATH  Previous-release rpm used for rollback validation.
  --out-dir DIR        Package/evidence output directory.
  --preflight          Check prerequisites without running certification.
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
    --preflight)
      PRE_FLIGHT=1
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

preflight_require() {
  local ok="$1"
  local message="$2"
  if [[ "$ok" != "1" ]]; then
    echo "[mem-service-linux-ops-ci] PREFLIGHT FAIL: $message" >&2
    return 1
  fi
  return 0
}

preflight_command() {
  local name="$1"
  if ! command -v "$name" >/dev/null; then
    echo "[mem-service-linux-ops-ci] PREFLIGHT FAIL: missing command: $name" >&2
    return 1
  fi
  return 0
}

run_preflight() {
  local failures=0

  preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  preflight_require "$([[ -r "$ROLLBACK_RPM" ]] && echo 1 || echo 0)" "rollback rpm not readable: $ROLLBACK_RPM" || failures=$((failures + 1))
  preflight_require "$([[ "$(uname -s)" == "Linux" ]] && echo 1 || echo 0)" "requires Linux host" || failures=$((failures + 1))
  preflight_require "$([[ -d /run/systemd/system ]] && echo 1 || echo 0)" "requires systemd runtime at /run/systemd/system" || failures=$((failures + 1))
  preflight_require "$([[ "$EUID" -eq 0 ]] && echo 1 || echo 0)" "requires root privileges for rpm/systemd checks" || failures=$((failures + 1))
  for tool in make rpmbuild rpm2cpio cpio rpm systemctl journalctl curl promtool; do
    preflight_command "$tool" || failures=$((failures + 1))
  done

  if [[ "$failures" -ne 0 ]]; then
    echo "[mem-service-linux-ops-ci] PREFLIGHT FAIL failures=$failures" >&2
    return 1
  fi
  printf '[mem-service-linux-ops-ci] PREFLIGHT PASS out=%s rollback_rpm=%s\n' "$OUT_DIR" "$ROLLBACK_RPM"
  return 0
}

printf '[mem-service-linux-ops-ci] RUN app=mem_service targets=linux-ops-deployment-smoke,linux-ops-certification-bundle out=%s rollback_rpm=%s\n' "$OUT_DIR" "$ROLLBACK_RPM"
if [[ "$PRE_FLIGHT" == "1" ]]; then
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  printf 'make -C %s PACKAGE_OUT_DIR=%s OPS_CERTIFICATION_ROLLBACK_RPM=%s linux-ops-deployment-smoke linux-ops-certification-bundle\n' "$APP_DIR" "$OUT_DIR" "$ROLLBACK_RPM"
  exit 0
fi

run_preflight
make -C "$APP_DIR" \
  "PACKAGE_OUT_DIR=$OUT_DIR" \
  "OPS_CERTIFICATION_ROLLBACK_RPM=$ROLLBACK_RPM" \
  linux-ops-deployment-smoke \
  linux-ops-certification-bundle

printf '[mem-service-linux-ops-ci] PASS evidence=%s marker=%s bundle=%s\n' \
  "$OUT_DIR/ops-certification-linux-ci.evidence" \
  "$OUT_DIR/ops-certification-upgrade-rollback.marker" \
  "$OUT_DIR/linqu-mem-service-ops-certification-bundle.tar"
