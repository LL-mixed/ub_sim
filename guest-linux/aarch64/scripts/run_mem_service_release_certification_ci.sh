#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APP_DIR="$ROOT_DIR/apps/mem_service"
APP_DIR_EXPLICIT=0
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/mem_service/release_certification_ci}"
ROLLBACK_RPM=""
RPM_FILE=""
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
  - installed_sdk/
  - linux_ops/ops-certification-upgrade-rollback.marker
  - linux_ops/ops-certification-linux-ci.evidence
  - linux_ops/linqu-mem-service-ops-certification-bundle.tar
  - remote_transport/remote-transport.evidence
  - remote_transport/linqu-mem-service-remote-transport-bundle.tar
  - release-certification.verify/

Options:
  --rollback-rpm PATH              Previous-release rpm used for rollback validation.
  --rpm-file PATH                  Current-release rpm used by installed Linux ops CI.
                                   Source-tree mode builds this via Makefile when omitted.
  --source tcp:IP:PORT             Producer TCP payload source for remote transport evidence.
  --producer-host HOST             Producer host identity recorded in evidence.
  --consumer-host HOST             Consumer host identity recorded in evidence.
  --network-partition-marker PATH  Marker proving partition fail-closed behavior.
  --out-dir DIR                    Release certification output directory.
  --app-dir DIR                    mem_service app directory override for source-tree builds.
  --preflight                      Check prerequisites without running certification.
  --dry-run                        Print the certification or preflight commands without running them.
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
    --rpm-file)
      if (( $# < 2 )); then
        echo "--rpm-file requires a path" >&2
        exit 2
      fi
      RPM_FILE="$2"
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
      APP_DIR_EXPLICIT=1
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

is_installed_script_context() {
  case "$SCRIPT_DIR" in
    */share/lingqu/mem_service/scripts)
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

print_sdk_gate_command() {
  if is_installed_script_context; then
    printf '%s/verify_mem_service_installed_sdk.sh --work-dir %s\n' \
      "$SCRIPT_DIR" "$SDK_OUT_DIR"
  else
    printf 'make -C %s DESTDIR=%s PREFIX=/usr PACKAGE_OUT_DIR=%s installed-sdk-pkgconfig-smoke installed-sdk-runtime-smoke\n' \
      "$APP_DIR" "$SDK_INSTALL_ROOT" "$SDK_PACKAGE_OUT_DIR"
  fi
}

run_sdk_gate() {
  if is_installed_script_context; then
    "$SCRIPT_DIR/verify_mem_service_installed_sdk.sh" --work-dir "$SDK_OUT_DIR"
  else
    make -C "$APP_DIR" \
      "DESTDIR=$SDK_INSTALL_ROOT" \
      "PREFIX=/usr" \
      "PACKAGE_OUT_DIR=$SDK_PACKAGE_OUT_DIR" \
      installed-sdk-pkgconfig-smoke \
      installed-sdk-runtime-smoke
  fi
}

remote_transport_app_args() {
  if is_installed_script_context && [[ "$APP_DIR_EXPLICIT" == "0" ]]; then
    return 0
  fi
  printf ' --app-dir %s' "$APP_DIR"
}

release_verify_app_args() {
  if is_installed_script_context && [[ "$APP_DIR_EXPLICIT" == "0" ]]; then
    return 0
  fi
  printf ' --app-dir %s' "$APP_DIR"
}

linux_ops_rpm_args() {
  if [[ -z "$RPM_FILE" ]]; then
    return 0
  fi
  printf ' --rpm-file %s' "$RPM_FILE"
}

linux_ops_app_args() {
  if is_installed_script_context && [[ "$APP_DIR_EXPLICIT" == "0" ]]; then
    return 0
  fi
  printf ' --app-dir %s' "$APP_DIR"
}

linux_ops_uses_installed_context() {
  is_installed_script_context && [[ "$APP_DIR_EXPLICIT" == "0" ]]
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
  local -a linux_ops_preflight_args
  local -a remote_transport_preflight_args

  if ! is_installed_script_context || [[ "$APP_DIR_EXPLICIT" == "1" ]]; then
    preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  fi
  preflight_require "$([[ -x "$SCRIPT_DIR/run_mem_service_linux_ops_ci.sh" ]] && echo 1 || echo 0)" "missing linux ops CI wrapper" || failures=$((failures + 1))
  preflight_require "$([[ -x "$SCRIPT_DIR/run_mem_service_remote_transport_ci.sh" ]] && echo 1 || echo 0)" "missing remote transport CI wrapper" || failures=$((failures + 1))
  preflight_require "$([[ -x "$SCRIPT_DIR/verify_mem_service_release_certification.sh" ]] && echo 1 || echo 0)" "missing release certification verifier" || failures=$((failures + 1))
  if is_installed_script_context; then
    preflight_require "$([[ -x "$SCRIPT_DIR/verify_mem_service_installed_sdk.sh" ]] && echo 1 || echo 0)" "missing installed SDK verifier" || failures=$((failures + 1))
  fi
  if linux_ops_uses_installed_context; then
    preflight_require "$([[ -r "$RPM_FILE" ]] && echo 1 || echo 0)" "current rpm not readable: $RPM_FILE" || failures=$((failures + 1))
  fi
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
  if ! is_installed_script_context || [[ "$APP_DIR_EXPLICIT" == "1" ]]; then
    preflight_command make || failures=$((failures + 1))
  fi
  for tool in cc pkg-config rpmbuild rpm2cpio cpio rpm systemctl journalctl curl promtool tar; do
    preflight_command "$tool" || failures=$((failures + 1))
  done

  if [[ "$failures" -eq 0 ]]; then
    linux_ops_preflight_args=(
      --rollback-rpm "$ROLLBACK_RPM"
      --out-dir "$OPS_OUT_DIR"
      --preflight
    )
    if [[ -n "$RPM_FILE" ]]; then
      linux_ops_preflight_args+=(--rpm-file "$RPM_FILE")
    fi
    if ! linux_ops_uses_installed_context; then
      linux_ops_preflight_args+=(--app-dir "$APP_DIR")
    fi
    if ! "$SCRIPT_DIR/run_mem_service_linux_ops_ci.sh" "${linux_ops_preflight_args[@]}"; then
      failures=$((failures + 1))
    fi

    remote_transport_preflight_args=(
      --source "$SOURCE"
      --producer-host "$PRODUCER_HOST"
      --consumer-host "$CONSUMER_HOST"
      --network-partition-marker "$PARTITION_MARKER"
      --out-dir "$REMOTE_OUT_DIR"
      --preflight
    )
    if ! is_installed_script_context || [[ "$APP_DIR_EXPLICIT" == "1" ]]; then
      remote_transport_preflight_args+=(--app-dir "$APP_DIR")
    fi
    if ! "$SCRIPT_DIR/run_mem_service_remote_transport_ci.sh" "${remote_transport_preflight_args[@]}"; then
      failures=$((failures + 1))
    fi
  fi

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
SDK_OUT_DIR="$OUT_DIR/installed_sdk"
SDK_INSTALL_ROOT="$SDK_OUT_DIR/install-root"
SDK_PACKAGE_OUT_DIR="$SDK_OUT_DIR/package"
RELEASE_VERIFY_WORK_DIR="$OUT_DIR/release-certification.verify"
OPS_BUNDLE="$OPS_OUT_DIR/linqu-mem-service-ops-certification-bundle.tar"
REMOTE_BUNDLE="$REMOTE_OUT_DIR/linqu-mem-service-remote-transport-bundle.tar"

printf '[mem-service-release-certification-ci] RUN out=%s rollback_rpm=%s source=%s producer=%s consumer=%s\n' \
  "$OUT_DIR" "$ROLLBACK_RPM" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST"

if [[ "$PRE_FLIGHT" == "1" ]]; then
  if [[ "$DRY_RUN" == "1" ]]; then
    printf 'preflight: release wrapper local checks\n'
    printf '%s/run_mem_service_linux_ops_ci.sh --rollback-rpm %s%s%s --out-dir %s --preflight\n' \
      "$SCRIPT_DIR" "$ROLLBACK_RPM" "$(linux_ops_rpm_args)" "$(linux_ops_app_args)" "$OPS_OUT_DIR"
    printf '%s/run_mem_service_remote_transport_ci.sh --source %s --producer-host %s --consumer-host %s --network-partition-marker %s --out-dir %s%s --preflight\n' \
      "$SCRIPT_DIR" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$PARTITION_MARKER" "$REMOTE_OUT_DIR" "$(remote_transport_app_args)"
    exit 0
  fi
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  print_sdk_gate_command
  printf '%s/run_mem_service_linux_ops_ci.sh --rollback-rpm %s%s%s --out-dir %s --dry-run\n' \
    "$SCRIPT_DIR" "$ROLLBACK_RPM" "$(linux_ops_rpm_args)" "$(linux_ops_app_args)" "$OPS_OUT_DIR"
  printf '%s/run_mem_service_remote_transport_ci.sh --source %s --producer-host %s --consumer-host %s --network-partition-marker %s --out-dir %s%s --dry-run\n' \
    "$SCRIPT_DIR" "$SOURCE" "$PRODUCER_HOST" "$CONSUMER_HOST" "$PARTITION_MARKER" "$REMOTE_OUT_DIR" "$(remote_transport_app_args)"
  printf '%s/verify_mem_service_release_certification.sh --ops-bundle-file %s --remote-transport-bundle-file %s%s --work-dir %s --dry-run\n' \
    "$SCRIPT_DIR" "$OPS_BUNDLE" "$REMOTE_BUNDLE" "$(release_verify_app_args)" "$RELEASE_VERIFY_WORK_DIR"
  exit 0
fi

if (! is_installed_script_context || [[ "$APP_DIR_EXPLICIT" == "1" ]]) && [[ ! -d "$APP_DIR" ]]; then
  echo "[mem-service-release-certification-ci] FAIL: app directory not found: $APP_DIR" >&2
  exit 1
fi
run_preflight
mkdir -p "$OUT_DIR"

run_sdk_gate

linux_ops_args=(
  --rollback-rpm "$ROLLBACK_RPM"
  --out-dir "$OPS_OUT_DIR"
)
if [[ -n "$RPM_FILE" ]]; then
  linux_ops_args+=(--rpm-file "$RPM_FILE")
fi
if ! linux_ops_uses_installed_context; then
  linux_ops_args+=(--app-dir "$APP_DIR")
fi
"$SCRIPT_DIR/run_mem_service_linux_ops_ci.sh" "${linux_ops_args[@]}"

remote_transport_args=(
  --source "$SOURCE"
  --producer-host "$PRODUCER_HOST"
  --consumer-host "$CONSUMER_HOST"
  --network-partition-marker "$PARTITION_MARKER"
  --out-dir "$REMOTE_OUT_DIR"
)
if ! is_installed_script_context || [[ "$APP_DIR_EXPLICIT" == "1" ]]; then
  remote_transport_args+=(--app-dir "$APP_DIR")
fi
"$SCRIPT_DIR/run_mem_service_remote_transport_ci.sh" "${remote_transport_args[@]}"

if is_installed_script_context && [[ "$APP_DIR_EXPLICIT" == "0" ]]; then
  "$SCRIPT_DIR/verify_mem_service_release_certification.sh" \
    --ops-bundle-file "$OPS_BUNDLE" \
    --remote-transport-bundle-file "$REMOTE_BUNDLE" \
    --work-dir "$RELEASE_VERIFY_WORK_DIR"
else
  "$SCRIPT_DIR/verify_mem_service_release_certification.sh" \
    --ops-bundle-file "$OPS_BUNDLE" \
    --remote-transport-bundle-file "$REMOTE_BUNDLE" \
    --app-dir "$APP_DIR" \
    --work-dir "$RELEASE_VERIFY_WORK_DIR"
fi

printf '[mem-service-release-certification-ci] PASS ops_bundle=%s remote_transport_bundle=%s verify_work_dir=%s\n' \
  "$OPS_BUNDLE" "$REMOTE_BUNDLE" "$RELEASE_VERIFY_WORK_DIR"
