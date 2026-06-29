#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
OUT_DIR="$ROOT_DIR/out/mem_service/remote_transport_ci"
SOURCE=""
PRODUCER_HOST=""
CONSUMER_HOST=""
PARTITION_MARKER=""
EVIDENCE_FILE=""
BUNDLE_FILE=""
STORAGE_ROOT=""
DRY_RUN=0
PRE_FLIGHT=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_remote_transport_ci.sh --source tcp:IP:PORT --producer-host HOST --consumer-host HOST --network-partition-marker PATH [options]

Runs the mem_service cross-host remote transport evidence wrapper.

Requirements:
  - Producer host serves a one-shot payload at --source.
  - Consumer host runs this script.
  - --source must use a non-loopback IPv4 address.
  - --producer-host and --consumer-host must be distinct.
  - The network partition marker must contain network_partition_fail_closed=pass.

Outputs:
  - remote-transport.evidence
  - linqu-mem-service-remote-transport-bundle.tar

Options:
  --source tcp:IP:PORT              Producer TCP payload source.
  --producer-host HOST              Producer host identity recorded in evidence.
  --consumer-host HOST              Consumer host identity recorded in evidence.
  --network-partition-marker PATH   Marker proving partition fail-closed behavior.
  --evidence-file PATH              Evidence output path.
  --bundle-file PATH                Bundle output path.
  --storage-root DIR                Temporary storage root used by the probe.
  --out-dir DIR                     Output directory for default evidence/storage paths.
  --app-dir DIR                     mem_service app directory containing linqu_mem_service_host.
  --preflight                       Check prerequisites without running certification.
  --dry-run                         Print commands without running them.
  -h, --help                        Show this help.
EOF
}

while (( $# > 0 )); do
  case "$1" in
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
    --evidence-file)
      if (( $# < 2 )); then
        echo "--evidence-file requires a path" >&2
        exit 2
      fi
      EVIDENCE_FILE="$2"
      shift 2
      ;;
    --bundle-file)
      if (( $# < 2 )); then
        echo "--bundle-file requires a path" >&2
        exit 2
      fi
      BUNDLE_FILE="$2"
      shift 2
      ;;
    --storage-root)
      if (( $# < 2 )); then
        echo "--storage-root requires a directory" >&2
        exit 2
      fi
      STORAGE_ROOT="$2"
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

if [[ -z "$SOURCE" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --source is required" >&2
  exit 2
fi
if [[ -z "$PRODUCER_HOST" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --producer-host is required" >&2
  exit 2
fi
if [[ -z "$CONSUMER_HOST" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --consumer-host is required" >&2
  exit 2
fi
if [[ -z "$PARTITION_MARKER" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --network-partition-marker is required" >&2
  exit 2
fi

if [[ -z "$EVIDENCE_FILE" ]]; then
  EVIDENCE_FILE="$OUT_DIR/remote-transport.evidence"
fi
if [[ -z "$BUNDLE_FILE" ]]; then
  BUNDLE_FILE="$OUT_DIR/linqu-mem-service-remote-transport-bundle.tar"
fi
if [[ -z "$STORAGE_ROOT" ]]; then
  STORAGE_ROOT="$OUT_DIR/storage"
fi

preflight_require() {
  local ok="$1"
  local message="$2"
  if [[ "$ok" != "1" ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: $message" >&2
    return 1
  fi
  return 0
}

preflight_command() {
  local name="$1"
  if ! command -v "$name" >/dev/null; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: missing command: $name" >&2
    return 1
  fi
  return 0
}

preflight_source() {
  local source="$1"
  local address="${source#tcp:}"
  address="${address%:*}"
  if [[ "$source" != tcp:*:* ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: --source must be tcp:IP:PORT" >&2
    return 1
  fi
  if [[ "$address" == "127."* || "$address" == "0.0.0.0" || "$address" == "localhost" ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: --source must be non-loopback IPv4: $source" >&2
    return 1
  fi
  return 0
}

run_preflight() {
  local failures=0

  preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  preflight_require "$([[ -r "$PARTITION_MARKER" ]] && echo 1 || echo 0)" "network partition marker not readable: $PARTITION_MARKER" || failures=$((failures + 1))
  if [[ -r "$PARTITION_MARKER" ]] && ! grep -q '^network_partition_fail_closed=pass$' "$PARTITION_MARKER"; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: network partition marker must contain network_partition_fail_closed=pass" >&2
    failures=$((failures + 1))
  fi
  preflight_source "$SOURCE" || failures=$((failures + 1))
  if [[ "$PRODUCER_HOST" == "$CONSUMER_HOST" ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL: producer and consumer hosts must differ" >&2
    failures=$((failures + 1))
  fi
  preflight_command make || failures=$((failures + 1))

  if [[ "$failures" -ne 0 ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL failures=$failures" >&2
    return 1
  fi
  printf '[mem-service-remote-transport-ci] PREFLIGHT PASS source=%s producer=%s consumer=%s evidence=%s\n' \
    "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$EVIDENCE_FILE"
  return 0
}

printf '[mem-service-remote-transport-ci] RUN source=%s producer=%s consumer=%s evidence=%s bundle=%s\n' "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$EVIDENCE_FILE" "$BUNDLE_FILE"
if [[ "$PRE_FLIGHT" == "1" ]]; then
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  printf 'make -C %s linqu_mem_service_host\n' "$APP_DIR"
  printf '%s/linqu_mem_service_host remote-transport-generate-evidence --source %s --producer-host %s --consumer-host %s --network-partition-marker %s --evidence-file %s --storage-root %s\n' "$APP_DIR" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$PARTITION_MARKER" "$EVIDENCE_FILE" "$STORAGE_ROOT"
  printf '%s/linqu_mem_service_host remote-transport-verify --evidence-file %s\n' "$APP_DIR" "$EVIDENCE_FILE"
  printf 'make -C %s PACKAGE_OUT_DIR=%s REMOTE_TRANSPORT_EVIDENCE=%s REMOTE_TRANSPORT_BUNDLE=%s remote-transport-certification-bundle remote-transport-certification-bundle-verify\n' "$APP_DIR" "$OUT_DIR" "$EVIDENCE_FILE" "$BUNDLE_FILE"
  exit 0
fi

if [[ ! -d "$APP_DIR" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: app directory not found: $APP_DIR" >&2
  exit 1
fi
if [[ ! -f "$PARTITION_MARKER" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: network partition marker not found: $PARTITION_MARKER" >&2
  exit 1
fi

run_preflight
mkdir -p "$(dirname "$EVIDENCE_FILE")" "$STORAGE_ROOT"
mkdir -p "$(dirname "$BUNDLE_FILE")"
if [[ ! -x "$APP_DIR/linqu_mem_service_host" ]]; then
  make -C "$APP_DIR" linqu_mem_service_host
fi

"$APP_DIR/linqu_mem_service_host" remote-transport-generate-evidence \
  --source "$SOURCE" \
  --producer-host "$PRODUCER_HOST" \
  --consumer-host "$CONSUMER_HOST" \
  --network-partition-marker "$PARTITION_MARKER" \
  --evidence-file "$EVIDENCE_FILE" \
  --storage-root "$STORAGE_ROOT"
"$APP_DIR/linqu_mem_service_host" remote-transport-verify --evidence-file "$EVIDENCE_FILE"
make -C "$APP_DIR" \
  PACKAGE_OUT_DIR="$OUT_DIR" \
  REMOTE_TRANSPORT_EVIDENCE="$EVIDENCE_FILE" \
  REMOTE_TRANSPORT_BUNDLE="$BUNDLE_FILE" \
  remote-transport-certification-bundle \
  remote-transport-certification-bundle-verify

printf '[mem-service-remote-transport-ci] PASS evidence=%s bundle=%s\n' "$EVIDENCE_FILE" "$BUNDLE_FILE"
