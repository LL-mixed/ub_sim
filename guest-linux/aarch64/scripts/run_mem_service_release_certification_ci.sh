#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/mem_service/release_certification_ci}"
ROLLBACK_RPM=""
SOURCE=""
PRODUCER_HOST=""
CONSUMER_HOST=""
PARTITION_MARKER=""
DRY_RUN=0
PRE_FLIGHT=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_release_certification_ci.sh --rollback-rpm PATH --source tcp:IP:PORT --producer-host HOST --consumer-host HOST --network-partition-marker PATH [options]

Runs the full mem_service release certification wrapper.

Requirements:
  - Linux host with systemd for the Linux ops phase
  - root privileges for rpm install/start/rollback checks
  - rpmbuild, rpm2cpio, cpio, rpm, curl, promtool
  - readable previous-release rollback rpm
  - producer host serves a one-shot non-loopback TCP payload source
  - producer and consumer host identities are distinct
  - network partition marker contains network_partition_fail_closed=pass

Outputs:
  - linux_ops/ops-certification-upgrade-rollback.marker
  - linux_ops/ops-certification-linux-ci.evidence
  - linux_ops/linqu-mem-service-ops-certification-bundle.tar
  - remote_transport/remote-transport.evidence
  - remote_transport/linqu-mem-service-remote-transport-bundle.tar
  - release-certification.verify/

Options:
  --rollback-rpm PATH              Previous-release rpm used for rollback validation.
  --source tcp:IP:PORT             Producer TCP payload source for remote transport evidence.
  --producer-host HOST             Producer host identity recorded in evidence.
  --consumer-host HOST             Consumer host identity recorded in evidence.
  --network-partition-marker PATH  Marker proving partition fail-closed behavior.
  --out-dir DIR                    Release certification output directory.
  --app-dir DIR                    mem_service app directory override for source-tree builds.
  --preflight                      Check prerequisites without running certification.
  --dry-run                        Print the certification commands without running them.
  -h, --help                       Show this help.
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
    --source)
      if (( $# < 2 )); then
        echo "--source requires a tcp:IP:PORT value" >&2
        exit 2
      fi
      SOURCE="$2"
      shift 2
      ;;
    --producer-host)
      if (( $# < 2 )); then
        echo "--producer-host requires a host value" >&2
        exit 2
      fi
      PRODUCER_HOST="$2"
      shift 2
      ;;
    --consumer-host)
      if (( $# < 2 )); then
        echo "--consumer-host requires a host value" >&2
        exit 2
      fi
      CONSUMER_HOST="$2"
      shift 2
      ;;
    --network-partition-marker)
      if (( $# < 2 )); then
        echo "--network-partition-marker requires a path" >&2
        exit 2
      fi
      PARTITION_MARKER="$2"
      shift 2
      ;;
    --out-dir)
      if (( $# < 2 )); then
        echo "--out-dir requires a directory" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift 2
      ;;
    --app-dir)
      if (( $# < 2 )); then
        echo "--app-dir requires a directory" >&2
        exit 2
      fi
      APP_DIR="$2"
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
  echo "[mem-service-release-certification-ci] FAIL: --rollback-rpm is required" >&2
  exit 2
fi
if [[ -z "$SOURCE" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: --source is required" >&2
  exit 2
fi
if [[ -z "$PRODUCER_HOST" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: --producer-host is required" >&2
  exit 2
fi
if [[ -z "$CONSUMER_HOST" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: --consumer-host is required" >&2
  exit 2
fi
if [[ -z "$PARTITION_MARKER" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: --network-partition-marker is required" >&2
  exit 2
fi

preflight_require() {
  local ok="$1"
  local message="$2"
  if [[ "$ok" != "1" ]]; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: $message" >&2
    return 1
  fi
  return 0
}

preflight_command() {
  local name="$1"
  if ! command -v "$name" >/dev/null; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: missing command: $name" >&2
    return 1
  fi
  return 0
}

preflight_source() {
  local source="$1"
  local address="${source#tcp:}"
  address="${address%:*}"
  if [[ "$source" != tcp:*:* ]]; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: --source must be tcp:IP:PORT" >&2
    return 1
  fi
  if [[ "$address" == "127."* || "$address" == "0.0.0.0" || "$address" == "localhost" ]]; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: --source must be non-loopback IPv4: $source" >&2
    return 1
  fi
  return 0
}

run_preflight() {
  local failures=0

  preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  preflight_require "$([[ -x "$SCRIPT_DIR/run_mem_service_linux_ops_ci.sh" ]] && echo 1 || echo 0)" "missing linux ops CI wrapper" || failures=$((failures + 1))
  preflight_require "$([[ -x "$SCRIPT_DIR/run_mem_service_remote_transport_ci.sh" ]] && echo 1 || echo 0)" "missing remote transport CI wrapper" || failures=$((failures + 1))
  preflight_require "$([[ -x "$SCRIPT_DIR/verify_mem_service_release_certification.sh" ]] && echo 1 || echo 0)" "missing release certification verifier" || failures=$((failures + 1))
  preflight_require "$([[ -r "$ROLLBACK_RPM" ]] && echo 1 || echo 0)" "rollback rpm not readable: $ROLLBACK_RPM" || failures=$((failures + 1))
  preflight_require "$([[ -r "$PARTITION_MARKER" ]] && echo 1 || echo 0)" "network partition marker not readable: $PARTITION_MARKER" || failures=$((failures + 1))
  if [[ -r "$PARTITION_MARKER" ]] && ! grep -q '^network_partition_fail_closed=pass$' "$PARTITION_MARKER"; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: network partition marker must contain network_partition_fail_closed=pass" >&2
    failures=$((failures + 1))
  fi
  preflight_source "$SOURCE" || failures=$((failures + 1))
  if [[ "$PRODUCER_HOST" == "$CONSUMER_HOST" ]]; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL: producer and consumer hosts must differ" >&2
    failures=$((failures + 1))
  fi
  preflight_require "$([[ "$(uname -s)" == "Linux" ]] && echo 1 || echo 0)" "requires Linux host" || failures=$((failures + 1))
  preflight_require "$([[ -d /run/systemd/system ]] && echo 1 || echo 0)" "requires systemd runtime at /run/systemd/system" || failures=$((failures + 1))
  preflight_require "$([[ "$EUID" -eq 0 ]] && echo 1 || echo 0)" "requires root privileges for rpm/systemd checks" || failures=$((failures + 1))
  for tool in make rpmbuild rpm2cpio cpio rpm curl promtool; do
    preflight_command "$tool" || failures=$((failures + 1))
  done

  if [[ "$failures" -ne 0 ]]; then
    echo "[mem-service-release-certification-ci] PREFLIGHT FAIL failures=$failures" >&2
    return 1
  fi
  printf '[mem-service-release-certification-ci] PREFLIGHT PASS out=%s rollback_rpm=%s source=%s\n' \
    "$OUT_DIR" "$ROLLBACK_RPM" "$SOURCE"
  return 0
}

OPS_OUT_DIR="$OUT_DIR/linux_ops"
REMOTE_OUT_DIR="$OUT_DIR/remote_transport"
RELEASE_VERIFY_WORK_DIR="$OUT_DIR/release-certification.verify"
OPS_BUNDLE="$OPS_OUT_DIR/linqu-mem-service-ops-certification-bundle.tar"
REMOTE_BUNDLE="$REMOTE_OUT_DIR/linqu-mem-service-remote-transport-bundle.tar"

printf '[mem-service-release-certification-ci] RUN out=%s rollback_rpm=%s source=%s producer=%s consumer=%s\n' \
  "$OUT_DIR" "$ROLLBACK_RPM" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST"

if [[ "$PRE_FLIGHT" == "1" ]]; then
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  printf '%s/run_mem_service_linux_ops_ci.sh --rollback-rpm %s --out-dir %s --dry-run\n' \
    "$SCRIPT_DIR" "$ROLLBACK_RPM" "$OPS_OUT_DIR"
  printf '%s/run_mem_service_remote_transport_ci.sh --source %s --producer-host %s --consumer-host %s --network-partition-marker %s --out-dir %s --app-dir %s --dry-run\n' \
    "$SCRIPT_DIR" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$PARTITION_MARKER" "$REMOTE_OUT_DIR" "$APP_DIR"
  printf '%s/verify_mem_service_release_certification.sh --ops-bundle-file %s --remote-transport-bundle-file %s --app-dir %s --work-dir %s --dry-run\n' \
    "$SCRIPT_DIR" "$OPS_BUNDLE" "$REMOTE_BUNDLE" "$APP_DIR" "$RELEASE_VERIFY_WORK_DIR"
  exit 0
fi

if [[ ! -d "$APP_DIR" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: app directory not found: $APP_DIR" >&2
  exit 1
fi
run_preflight
mkdir -p "$OUT_DIR"

"$SCRIPT_DIR/run_mem_service_linux_ops_ci.sh" \
  --rollback-rpm "$ROLLBACK_RPM" \
  --out-dir "$OPS_OUT_DIR"

"$SCRIPT_DIR/run_mem_service_remote_transport_ci.sh" \
  --source "$SOURCE" \
  --producer-host "$PRODUCER_HOST" \
  --consumer-host "$CONSUMER_HOST" \
  --network-partition-marker "$PARTITION_MARKER" \
  --out-dir "$REMOTE_OUT_DIR" \
  --app-dir "$APP_DIR"

"$SCRIPT_DIR/verify_mem_service_release_certification.sh" \
  --ops-bundle-file "$OPS_BUNDLE" \
  --remote-transport-bundle-file "$REMOTE_BUNDLE" \
  --app-dir "$APP_DIR" \
  --work-dir "$RELEASE_VERIFY_WORK_DIR"

printf '[mem-service-release-certification-ci] PASS ops_bundle=%s remote_transport_bundle=%s verify_work_dir=%s\n' \
  "$OPS_BUNDLE" "$REMOTE_BUNDLE" "$RELEASE_VERIFY_WORK_DIR"
