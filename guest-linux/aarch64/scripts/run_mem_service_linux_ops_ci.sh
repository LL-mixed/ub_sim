#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_APP_DIR="$ROOT_DIR/apps/mem_service"
APP_DIR=""
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out/mem_service/linux_ops_ci}"
ROLLBACK_RPM=""
RPM_FILE=""
HOST_BIN=""
BUNDLE_MODE=""
INSTALLED_DATA_DIR=""
OPS_EVIDENCE=""
OPS_MARKER=""
OPS_BUNDLE_ROOT=""
OPS_BUNDLE=""
OPS_BUNDLE_MANIFEST=""
OPS_BUNDLE_VERIFY_ROOT=""
DRY_RUN=0
PRE_FLIGHT=0

usage() {
  cat <<'EOF'
Usage: run_mem_service_linux_ops_ci.sh --rollback-rpm PATH [--rpm-file PATH] [--app-dir DIR] [--out-dir DIR] [--preflight] [--dry-run]

Runs the mem_service real-Linux deployment certification wrapper.

Requirements:
  - Linux host with systemd
  - root privileges
  - rpmbuild, rpm2cpio, cpio, rpm, curl, promtool
  - readable current-release rpm when running from an installed package
  - readable previous-release rollback rpm

Outputs:
  - ops-certification-upgrade-rollback.marker
  - ops-certification-linux-ci.evidence
  - linqu-mem-service-ops-certification-bundle.tar

Options:
  --rollback-rpm PATH  Previous-release rpm used for rollback validation.
  --rpm-file PATH      Current-release rpm to certify when running from an
                       installed package. Source-tree mode builds this via
                       Makefile when omitted.
  --app-dir DIR        mem_service app directory override for source-tree builds.
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
    --rpm-file)
      if (( $# < 2 )); then
        echo "--rpm-file requires a path" >&2
        exit 2
      fi
      RPM_FILE="$2"
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

OPS_EVIDENCE="$OUT_DIR/ops-certification-linux-ci.evidence"
OPS_MARKER="$OUT_DIR/ops-certification-upgrade-rollback.marker"
OPS_BUNDLE_ROOT="$OUT_DIR/ops-certification-bundle"
OPS_BUNDLE="$OUT_DIR/linqu-mem-service-ops-certification-bundle.tar"
OPS_BUNDLE_MANIFEST="$OPS_BUNDLE_ROOT/ops-certification-bundle.manifest"
OPS_BUNDLE_VERIFY_ROOT="$OUT_DIR/ops-certification-bundle.verify"

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

run_preflight() {
  local failures=0

  if [[ "$BUNDLE_MODE" == "make" ]]; then
    preflight_require "$([[ -d "$APP_DIR" ]] && echo 1 || echo 0)" "app directory not found: $APP_DIR" || failures=$((failures + 1))
  else
    preflight_require "$([[ -x "$HOST_BIN" ]] && echo 1 || echo 0)" "host binary not executable: $HOST_BIN" || failures=$((failures + 1))
    preflight_require "$([[ -r "$RPM_FILE" ]] && echo 1 || echo 0)" "current rpm not readable: $RPM_FILE" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/release-manifest.txt" ]] && echo 1 || echo 0)" "installed release manifest not found: $INSTALLED_DATA_DIR/release-manifest.txt" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/package-manifest.txt" ]] && echo 1 || echo 0)" "installed package manifest not found: $INSTALLED_DATA_DIR/package-manifest.txt" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/ops-certification-policy.txt" ]] && echo 1 || echo 0)" "installed ops policy not found: $INSTALLED_DATA_DIR/ops-certification-policy.txt" || failures=$((failures + 1))
    preflight_require "$([[ -f "$INSTALLED_DATA_DIR/deploy/linqu_mem_service.prometheus-alerts.yml" ]] && echo 1 || echo 0)" "installed alert rules not found: $INSTALLED_DATA_DIR/deploy/linqu_mem_service.prometheus-alerts.yml" || failures=$((failures + 1))
  fi
  preflight_require "$([[ -r "$ROLLBACK_RPM" ]] && echo 1 || echo 0)" "rollback rpm not readable: $ROLLBACK_RPM" || failures=$((failures + 1))
  preflight_require "$([[ "$(uname -s)" == "Linux" ]] && echo 1 || echo 0)" "requires Linux host" || failures=$((failures + 1))
  preflight_require "$([[ -d /run/systemd/system ]] && echo 1 || echo 0)" "requires systemd runtime at /run/systemd/system" || failures=$((failures + 1))
  preflight_require "$([[ "$EUID" -eq 0 ]] && echo 1 || echo 0)" "requires root privileges for rpm/systemd checks" || failures=$((failures + 1))
  if [[ "$BUNDLE_MODE" == "make" ]]; then
    preflight_command make || failures=$((failures + 1))
  fi
  for tool in rpmbuild rpm2cpio cpio rpm systemctl journalctl curl promtool; do
    preflight_command "$tool" || failures=$((failures + 1))
  done
  preflight_command tar || failures=$((failures + 1))

  if [[ "$failures" -ne 0 ]]; then
    echo "[mem-service-linux-ops-ci] PREFLIGHT FAIL failures=$failures" >&2
    return 1
  fi
  printf '[mem-service-linux-ops-ci] PREFLIGHT PASS out=%s rollback_rpm=%s\n' "$OUT_DIR" "$ROLLBACK_RPM"
  return 0
}

run_systemd_metrics_check() {
  systemctl daemon-reload
  systemctl restart linqu_mem_service.service
  systemctl restart linqu_mem_service.host.service
  systemctl is-active --quiet linqu_mem_service.service
  systemctl is-active --quiet linqu_mem_service.host.service
  curl -fsS http://127.0.0.1:9900/metrics | grep -q 'lingqu_mem_service_'
  curl -fsS http://127.0.0.1:9901/metrics | grep -q 'lingqu_mem_service_'
}

run_installed_deployment() {
  mkdir -p "$OUT_DIR"
  rpm -Uvh --replacepkgs "$RPM_FILE"
  run_systemd_metrics_check
  rpm -Uvh --oldpackage --replacepkgs "$ROLLBACK_RPM"
  run_systemd_metrics_check
  rpm -Uvh --replacepkgs "$RPM_FILE"
  run_systemd_metrics_check
  printf '%s\n' \
    'upgrade_rollback_deployment_smoke=pass' \
    "current_rpm=$RPM_FILE" \
    "rollback_rpm=$ROLLBACK_RPM" \
    > "$OPS_MARKER"
  rpm -Uvh --replacepkgs "$RPM_FILE"
  run_systemd_metrics_check
  promtool check rules /usr/share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml
  "$HOST_BIN" ops-certification-linux-ci-smoke \
    --rpm-file "$RPM_FILE" \
    --upgrade-rollback-marker "$OPS_MARKER" \
    --evidence-file "$OPS_EVIDENCE"
}

create_installed_bundle() {
  "$HOST_BIN" ops-certification-verify --evidence-file "$OPS_EVIDENCE"
  rm -rf "$OPS_BUNDLE_ROOT"
  mkdir -p "$OPS_BUNDLE_ROOT"
  cp "$OPS_EVIDENCE" "$OPS_BUNDLE_ROOT/ops-certification-linux-ci.evidence"
  cp "$OPS_MARKER" "$OPS_BUNDLE_ROOT/ops-certification-upgrade-rollback.marker"
  cp "$RPM_FILE" "$OPS_BUNDLE_ROOT/$(basename "$RPM_FILE")"
  cp "$INSTALLED_DATA_DIR/release-manifest.txt" "$OPS_BUNDLE_ROOT/release-manifest.txt"
  cp "$INSTALLED_DATA_DIR/package-manifest.txt" "$OPS_BUNDLE_ROOT/package-manifest.txt"
  cp "$INSTALLED_DATA_DIR/ops-certification-policy.txt" "$OPS_BUNDLE_ROOT/ops-certification-policy.txt"
  printf '%s\n' \
    'bundle_schema=linqu-mem-service-ops-certification-bundle-v1' \
    'bundle_gate=linux-ops-certification-bundle' \
    'evidence_verify_gate=linux-ops-evidence-verify' \
    'evidence=ops-certification-linux-ci.evidence' \
    'upgrade_rollback_marker=ops-certification-upgrade-rollback.marker' \
    "rpm=$(basename "$RPM_FILE")" \
    'release_manifest=release-manifest.txt' \
    'package_manifest=package-manifest.txt' \
    'ops_certification_policy=ops-certification-policy.txt' \
    > "$OPS_BUNDLE_MANIFEST"
  tar -C "$OPS_BUNDLE_ROOT" -cf "$OPS_BUNDLE" .
  test -f "$OPS_BUNDLE"
  "$SCRIPT_DIR/verify_mem_service_ops_certification_bundle.sh" \
    --bundle-file "$OPS_BUNDLE" \
    --work-dir "$OPS_BUNDLE_VERIFY_ROOT"
}

resolve_host_context

printf '[mem-service-linux-ops-ci] RUN app=mem_service mode=%s targets=linux-ops-deployment-smoke,linux-ops-certification-bundle out=%s rollback_rpm=%s\n' "$BUNDLE_MODE" "$OUT_DIR" "$ROLLBACK_RPM"
if [[ "$PRE_FLIGHT" == "1" ]]; then
  run_preflight
  exit $?
fi

if [[ "$DRY_RUN" == "1" ]]; then
  if [[ "$BUNDLE_MODE" == "make" ]]; then
    printf 'make -C %s PACKAGE_OUT_DIR=%s OPS_CERTIFICATION_ROLLBACK_RPM=%s linux-ops-deployment-smoke linux-ops-certification-bundle\n' "$APP_DIR" "$OUT_DIR" "$ROLLBACK_RPM"
  else
    current_rpm="$RPM_FILE"
    if [[ -z "$current_rpm" ]]; then
      current_rpm='<required-current-rpm>'
    fi
    printf 'rpm -Uvh --replacepkgs %s\n' "$current_rpm"
    printf 'systemctl daemon-reload && systemctl restart linqu_mem_service.service linqu_mem_service.host.service\n'
    printf 'curl -fsS http://127.0.0.1:9900/metrics && curl -fsS http://127.0.0.1:9901/metrics\n'
    printf 'rpm -Uvh --oldpackage --replacepkgs %s\n' "$ROLLBACK_RPM"
    printf 'printf upgrade_rollback_deployment_smoke=pass > %s\n' "$OPS_MARKER"
    printf 'promtool check rules /usr/share/lingqu/mem_service/deploy/linqu_mem_service.prometheus-alerts.yml\n'
    printf '%s ops-certification-linux-ci-smoke --rpm-file %s --upgrade-rollback-marker %s --evidence-file %s\n' "$HOST_BIN" "$current_rpm" "$OPS_MARKER" "$OPS_EVIDENCE"
    printf '%s ops-certification-verify --evidence-file %s\n' "$HOST_BIN" "$OPS_EVIDENCE"
    printf 'tar -C %s -cf %s .\n' "$OPS_BUNDLE_ROOT" "$OPS_BUNDLE"
    printf '%s/verify_mem_service_ops_certification_bundle.sh --bundle-file %s --work-dir %s\n' "$SCRIPT_DIR" "$OPS_BUNDLE" "$OPS_BUNDLE_VERIFY_ROOT"
  fi
  exit 0
fi

run_preflight
if [[ "$BUNDLE_MODE" == "make" ]]; then
  make -C "$APP_DIR" \
    "PACKAGE_OUT_DIR=$OUT_DIR" \
    "OPS_CERTIFICATION_ROLLBACK_RPM=$ROLLBACK_RPM" \
    linux-ops-deployment-smoke \
    linux-ops-certification-bundle
else
  run_installed_deployment
  create_installed_bundle
fi

printf '[mem-service-linux-ops-ci] PASS evidence=%s marker=%s bundle=%s\n' \
  "$OPS_EVIDENCE" \
  "$OPS_MARKER" \
  "$OPS_BUNDLE"
