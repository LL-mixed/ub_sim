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

OPS_OUT_DIR="$OUT_DIR/linux_ops"
REMOTE_OUT_DIR="$OUT_DIR/remote_transport"
RELEASE_VERIFY_WORK_DIR="$OUT_DIR/release-certification.verify"
OPS_BUNDLE="$OPS_OUT_DIR/linqu-mem-service-ops-certification-bundle.tar"
REMOTE_BUNDLE="$REMOTE_OUT_DIR/linqu-mem-service-remote-transport-bundle.tar"

printf '[mem-service-release-certification-ci] RUN out=%s rollback_rpm=%s source=%s producer=%s consumer=%s\n' \
  "$OUT_DIR" "$ROLLBACK_RPM" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST"

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
