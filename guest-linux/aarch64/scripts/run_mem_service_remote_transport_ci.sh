#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_APP_DIR="$ROOT_DIR/apps/mem_service"
APP_DIR=""
OUT_DIR="$ROOT_DIR/out/mem_service/remote_transport_ci"
SOURCE=""
PRODUCER_HOST=""
CONSUMER_HOST=""
PARTITION_MARKER=""
EVIDENCE_FILE=""
BUNDLE_FILE=""
STORAGE_ROOT=""
HOST_BIN=""
PRODUCER_SSH=""
PRODUCER_BIN=""
PRODUCER_PAYLOAD_LEN="4096"
PRODUCER_LOG=""
PRODUCER_PID=0
BUNDLE_MODE=""
INSTALLED_DATA_DIR=""
REMOTE_TRANSPORT_BUNDLE_ROOT=""
REMOTE_TRANSPORT_BUNDLE_MANIFEST=""
REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT=""
DRY_RUN=0
PRE_FLIGHT=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_remote_transport_ci.sh --source tcp:IP:PORT --producer-host HOST --consumer-host HOST --network-partition-marker PATH [options]

Runs the mem_service cross-host remote transport evidence wrapper.

Requirements:
  - Producer host serves a one-shot payload at --source. Use
    linqu_mem_service_host remote-transport-serve-fixture --listen tcp:IP:PORT
    --payload-len 4096 on the producer host.
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
  --producer-ssh HOST               Start the payload source through ssh before probing.
                                    Uses non-interactive ssh with a 10s connect timeout.
  --producer-bin PATH               Producer-side linqu_mem_service_host path.
                                    Required with --producer-ssh in source-tree mode.
  --producer-payload-len BYTES      Producer payload length. Defaults to 4096.
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
    --producer-ssh)
      if (( $# < 2 )); then
        echo "--producer-ssh requires a host value" >&2
        exit 2
      fi
      PRODUCER_SSH="$2"
      shift 2
      ;;
    --producer-bin)
      if (( $# < 2 )); then
        echo "--producer-bin requires a path" >&2
        exit 2
      fi
      PRODUCER_BIN="$2"
      shift 2
      ;;
    --producer-payload-len)
      if (( $# < 2 )); then
        echo "--producer-payload-len requires a byte count" >&2
        exit 2
      fi
      PRODUCER_PAYLOAD_LEN="$2"
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
REMOTE_TRANSPORT_BUNDLE_ROOT="$OUT_DIR/remote-transport-bundle"
REMOTE_TRANSPORT_BUNDLE_MANIFEST="$REMOTE_TRANSPORT_BUNDLE_ROOT/remote-transport-bundle.manifest"
REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT="$OUT_DIR/remote-transport-bundle.verify"
PRODUCER_LOG="$OUT_DIR/remote-transport-producer.log"

if [[ "$PRODUCER_PAYLOAD_LEN" != <-> || "$PRODUCER_PAYLOAD_LEN" -lt 1 ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --producer-payload-len must be a positive integer" >&2
  exit 2
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

installed_prefix() {
  cd "$SCRIPT_DIR/../../../.." && pwd
}

resolve_host_context() {
  local installed_root
  local installed_host
  if [[ -n "$APP_DIR" ]]; then
    HOST_BIN="$APP_DIR/linqu_mem_service_host"
    BUNDLE_MODE="make"
    return
  fi

  installed_root="$(installed_prefix)"
  installed_host="$installed_root/libexec/lingqu/mem_service/linqu_mem_service_host"
  if [[ -x "$installed_host" ]]; then
    HOST_BIN="$installed_host"
    INSTALLED_DATA_DIR="$installed_root/share/lingqu/mem_service"
    BUNDLE_MODE="installed"
    return
  fi

  APP_DIR="$DEFAULT_APP_DIR"
  HOST_BIN="$APP_DIR/linqu_mem_service_host"
  BUNDLE_MODE="make"
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

  if [[ "$BUNDLE_MODE" == "make" ]]; then
    preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  else
    preflight_require "$([[ -x "$HOST_BIN" ]] && echo 1 || echo 0)" "host binary not executable: $HOST_BIN" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/release-manifest.txt" ]] && echo 1 || echo 0)" "installed release manifest not found: $INSTALLED_DATA_DIR/release-manifest.txt" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/package-manifest.txt" ]] && echo 1 || echo 0)" "installed package manifest not found: $INSTALLED_DATA_DIR/package-manifest.txt" || failures=$((failures + 1))
  fi
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
  if [[ "$BUNDLE_MODE" == "make" ]]; then
    preflight_command make || failures=$((failures + 1))
  else
    preflight_command tar || failures=$((failures + 1))
  fi
  if [[ -n "$PRODUCER_SSH" ]]; then
    preflight_command ssh || failures=$((failures + 1))
  fi

  if [[ "$failures" -ne 0 ]]; then
    echo "[mem-service-remote-transport-ci] PREFLIGHT FAIL failures=$failures" >&2
    return 1
  fi
  printf '[mem-service-remote-transport-ci] PREFLIGHT PASS source=%s producer=%s consumer=%s evidence=%s\n' \
    "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$EVIDENCE_FILE"
  return 0
}

create_installed_bundle() {
  rm -rf "$REMOTE_TRANSPORT_BUNDLE_ROOT"
  mkdir -p "$REMOTE_TRANSPORT_BUNDLE_ROOT"
  cp "$EVIDENCE_FILE" "$REMOTE_TRANSPORT_BUNDLE_ROOT/remote-transport.evidence"
  cp "$INSTALLED_DATA_DIR/release-manifest.txt" "$REMOTE_TRANSPORT_BUNDLE_ROOT/release-manifest.txt"
  cp "$INSTALLED_DATA_DIR/package-manifest.txt" "$REMOTE_TRANSPORT_BUNDLE_ROOT/package-manifest.txt"
  printf '%s\n' \
    'bundle_schema=linqu-mem-service-remote-transport-bundle-v1' \
    'bundle_gate=remote-transport-certification-bundle' \
    'evidence_verify_gate=remote-transport-evidence-verify' \
    'evidence=remote-transport.evidence' \
    'release_manifest=release-manifest.txt' \
    'package_manifest=package-manifest.txt' \
    > "$REMOTE_TRANSPORT_BUNDLE_MANIFEST"
  tar -C "$REMOTE_TRANSPORT_BUNDLE_ROOT" -cf "$BUNDLE_FILE" .
  test -f "$BUNDLE_FILE"
}

cleanup_producer() {
  if [[ "$PRODUCER_PID" -ne 0 ]]; then
    kill "$PRODUCER_PID" >/dev/null 2>&1 || true
    wait "$PRODUCER_PID" >/dev/null 2>&1 || true
    PRODUCER_PID=0
  fi
}

start_producer_if_requested() {
  local producer_bin
  local i

  if [[ -z "$PRODUCER_SSH" ]]; then
    return 0
  fi
  producer_bin="$PRODUCER_BIN"
  if [[ -z "$producer_bin" ]]; then
    producer_bin="$HOST_BIN"
  fi
  mkdir -p "$(dirname "$PRODUCER_LOG")"
  rm -f "$PRODUCER_LOG"
  printf '[mem-service-remote-transport-ci] producer start ssh=%s listen=%s payload_len=%s log=%s\n' \
    "$PRODUCER_SSH" "$SOURCE" "$PRODUCER_PAYLOAD_LEN" "$PRODUCER_LOG"
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$PRODUCER_SSH" \
    "$producer_bin" remote-transport-serve-fixture \
    --listen "$SOURCE" \
    --payload-len "$PRODUCER_PAYLOAD_LEN" \
    > "$PRODUCER_LOG" 2>&1 &
  PRODUCER_PID=$!
  i=0
  while ! grep -q 'remote-transport-serve-fixture: status=ready' "$PRODUCER_LOG" 2>/dev/null; do
    if ! kill -0 "$PRODUCER_PID" >/dev/null 2>&1; then
      cat "$PRODUCER_LOG" >&2
      echo "[mem-service-remote-transport-ci] FAIL: producer exited before ready" >&2
      PRODUCER_PID=0
      return 1
    fi
    i=$((i + 1))
    if [[ "$i" -ge 100 ]]; then
      cat "$PRODUCER_LOG" >&2
      echo "[mem-service-remote-transport-ci] FAIL: producer did not become ready" >&2
      return 1
    fi
    sleep 0.1
  done
  return 0
}

wait_for_producer_if_requested() {
  if [[ "$PRODUCER_PID" -eq 0 ]]; then
    return 0
  fi
  if ! wait "$PRODUCER_PID"; then
    PRODUCER_PID=0
    cat "$PRODUCER_LOG" >&2
    echo "[mem-service-remote-transport-ci] FAIL: producer exited with error" >&2
    return 1
  fi
  PRODUCER_PID=0
  if ! grep -q 'remote-transport-serve-fixture: status=done' "$PRODUCER_LOG"; then
    cat "$PRODUCER_LOG" >&2
    echo "[mem-service-remote-transport-ci] FAIL: producer did not finish cleanly" >&2
    return 1
  fi
  return 0
}

resolve_host_context
if [[ -n "$PRODUCER_SSH" && -z "$PRODUCER_BIN" && "$BUNDLE_MODE" == "make" ]]; then
  echo "[mem-service-remote-transport-ci] FAIL: --producer-bin is required with --producer-ssh in source-tree mode" >&2
  exit 2
fi

printf '[mem-service-remote-transport-ci] RUN source=%s producer=%s consumer=%s evidence=%s bundle=%s\n' "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$EVIDENCE_FILE" "$BUNDLE_FILE"
if [[ "$PRE_FLIGHT" == "1" ]]; then
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  if [[ "$BUNDLE_MODE" == "make" ]]; then
    printf 'make -C %s linqu_mem_service_host\n' "$APP_DIR"
  fi
  if [[ -n "$PRODUCER_SSH" ]]; then
    producer_bin="$PRODUCER_BIN"
    if [[ -z "$producer_bin" ]]; then
      producer_bin="$HOST_BIN"
    fi
    printf 'ssh -o BatchMode=yes -o ConnectTimeout=10 %s %s remote-transport-serve-fixture --listen %s --payload-len %s\n' "$PRODUCER_SSH" "$producer_bin" "$SOURCE" "$PRODUCER_PAYLOAD_LEN"
  else
    printf '# producer: %s remote-transport-serve-fixture --listen %s --payload-len %s\n' "$HOST_BIN" "$SOURCE" "$PRODUCER_PAYLOAD_LEN"
  fi
  printf '%s remote-transport-generate-evidence --source %s --producer-host %s --consumer-host %s --network-partition-marker %s --evidence-file %s --storage-root %s\n' "$HOST_BIN" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$PARTITION_MARKER" "$EVIDENCE_FILE" "$STORAGE_ROOT"
  printf '%s remote-transport-verify --evidence-file %s\n' "$HOST_BIN" "$EVIDENCE_FILE"
  if [[ "$BUNDLE_MODE" == "make" ]]; then
    printf 'make -C %s PACKAGE_OUT_DIR=%s REMOTE_TRANSPORT_EVIDENCE=%s REMOTE_TRANSPORT_BUNDLE=%s remote-transport-certification-bundle remote-transport-certification-bundle-verify\n' "$APP_DIR" "$OUT_DIR" "$EVIDENCE_FILE" "$BUNDLE_FILE"
  else
    printf 'tar -C %s -cf %s .\n' "$REMOTE_TRANSPORT_BUNDLE_ROOT" "$BUNDLE_FILE"
    printf '%s/verify_mem_service_remote_transport_bundle.sh --bundle-file %s --work-dir %s\n' "$SCRIPT_DIR" "$BUNDLE_FILE" "$REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT"
  fi
  exit 0
fi

if [[ "$BUNDLE_MODE" == "make" && ! -d "$APP_DIR" ]]; then
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
if [[ "$BUNDLE_MODE" == "make" && ! -x "$HOST_BIN" ]]; then
  make -C "$APP_DIR" linqu_mem_service_host
fi

trap cleanup_producer EXIT INT TERM
start_producer_if_requested
"$HOST_BIN" remote-transport-generate-evidence \
  --source "$SOURCE" \
  --producer-host "$PRODUCER_HOST" \
  --consumer-host "$CONSUMER_HOST" \
  --network-partition-marker "$PARTITION_MARKER" \
  --evidence-file "$EVIDENCE_FILE" \
  --storage-root "$STORAGE_ROOT"
wait_for_producer_if_requested
"$HOST_BIN" remote-transport-verify --evidence-file "$EVIDENCE_FILE"
if [[ "$BUNDLE_MODE" == "make" ]]; then
  make -C "$APP_DIR" \
    PACKAGE_OUT_DIR="$OUT_DIR" \
    REMOTE_TRANSPORT_EVIDENCE="$EVIDENCE_FILE" \
    REMOTE_TRANSPORT_BUNDLE="$BUNDLE_FILE" \
    remote-transport-certification-bundle \
    remote-transport-certification-bundle-verify
else
  create_installed_bundle
  "$SCRIPT_DIR/verify_mem_service_remote_transport_bundle.sh" \
    --bundle-file "$BUNDLE_FILE" \
    --work-dir "$REMOTE_TRANSPORT_BUNDLE_VERIFY_ROOT"
fi

printf '[mem-service-remote-transport-ci] PASS evidence=%s bundle=%s\n' "$EVIDENCE_FILE" "$BUNDLE_FILE"
