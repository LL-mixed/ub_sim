#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_ROOT=""
INSTALL_PREFIX=""
DRY_RUN=0
NO_RUNTIME=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_installed_layout.sh [--root DIR] [--prefix DIR] [--no-runtime] [--dry-run]

Verifies an installed mem_service layout without requiring a source checkout.

Options:
  --root DIR      Installed root, useful for DESTDIR validation. Defaults to
                  deriving the root from this script location.
  --prefix DIR    Installed prefix inside --root. Defaults to deriving the
                  prefix from this script location.
  --no-runtime    Check files only; do not execute linqu_mem_service_host.
  --dry-run       Print the checks without running them.
  -h, --help      Show this help.
EOF
}

fail() {
  echo "[mem-service-installed-layout] FAIL: $*" >&2
  exit 1
}

derive_paths() {
  if [ -z "$INSTALL_PREFIX" ]; then
    case "$SCRIPT_DIR" in
      */share/lingqu/mem_service/scripts)
        INSTALL_PREFIX=${SCRIPT_DIR%/share/lingqu/mem_service/scripts}
        ;;
      *)
        fail "cannot derive prefix from script directory: $SCRIPT_DIR"
        ;;
    esac
  fi

  if [ -z "$INSTALL_ROOT" ]; then
    case "$INSTALL_PREFIX" in
      */usr/local)
        INSTALL_ROOT=${INSTALL_PREFIX%/usr/local}
        ;;
      */usr)
        INSTALL_ROOT=${INSTALL_PREFIX%/usr}
        ;;
      *)
        INSTALL_ROOT=/
        ;;
    esac
    if [ -z "$INSTALL_ROOT" ]; then
      INSTALL_ROOT=/
    fi
  fi
}

path_join() {
  case "$1" in
    /)
      printf '/%s\n' "$2"
      ;;
    *)
      printf '%s/%s\n' "$1" "$2"
      ;;
  esac
}

require_file() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'test -f %s\n' "$1"
    return
  fi
  [ -f "$1" ] || fail "missing file: $1"
}

require_executable() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'test -x %s\n' "$1"
    return
  fi
  [ -x "$1" ] || fail "missing executable: $1"
}

require_grep() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'grep -q %s %s\n' "$2" "$1"
    return
  fi
  grep -q "$2" "$1" || fail "missing pattern in $1: $2"
}

run_host_fixture() {
  if [ "$NO_RUNTIME" -eq 1 ]; then
    return
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '%s %s\n' "$HOST_BIN" "$1"
    return
  fi
  "$HOST_BIN" "$1" >/dev/null || fail "host fixture failed: $1"
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --root)
      [ "$#" -ge 2 ] || fail "--root requires a path"
      INSTALL_ROOT=$2
      shift 2
      ;;
    --prefix)
      [ "$#" -ge 2 ] || fail "--prefix requires a path"
      INSTALL_PREFIX=$2
      shift 2
      ;;
    --no-runtime)
      NO_RUNTIME=1
      shift
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

derive_paths

DATA_DIR=$(path_join "$INSTALL_PREFIX" "share/lingqu/mem_service")
SCRIPT_INSTALL_DIR=$(path_join "$DATA_DIR" "scripts")
CONFIG_DIR=$(path_join "$DATA_DIR" "config")
DEPLOY_DIR=$(path_join "$DATA_DIR" "deploy")
HOST_BIN=$(path_join "$INSTALL_PREFIX" "libexec/lingqu/mem_service/linqu_mem_service_host")
CORE_BIN=$(path_join "$INSTALL_PREFIX" "bin/linqu_mem_service")
SYSTEM_CONFIG_DIR=$(path_join "$INSTALL_ROOT" "etc/lingqu/mem_service")
SYSTEMD_DIR=$(path_join "$INSTALL_ROOT" "usr/lib/systemd/system")

require_executable "$CORE_BIN"
require_executable "$HOST_BIN"
require_file "$(path_join "$DATA_DIR" "release-manifest.txt")"
require_file "$(path_join "$DATA_DIR" "package-manifest.txt")"
require_file "$(path_join "$DATA_DIR" "wire-schema.txt")"
require_file "$(path_join "$DATA_DIR" "admin-output-schema.txt")"
require_file "$(path_join "$DATA_DIR" "upgrade-rollback-policy.txt")"
require_file "$(path_join "$DATA_DIR" "ops-certification-policy.txt")"
require_file "$(path_join "$CONFIG_DIR" "mem_service.runtime.conf")"
require_file "$(path_join "$CONFIG_DIR" "mem_service.host.runtime.conf")"
require_file "$(path_join "$SYSTEM_CONFIG_DIR" "mem_service.conf")"
require_file "$(path_join "$SYSTEM_CONFIG_DIR" "mem_service.host.conf")"
require_file "$(path_join "$DEPLOY_DIR" "linqu_mem_service.service")"
require_file "$(path_join "$DEPLOY_DIR" "linqu_mem_service.host.service")"
require_file "$(path_join "$DEPLOY_DIR" "linqu_mem_service.prometheus-alerts.yml")"
require_file "$(path_join "$SYSTEMD_DIR" "linqu_mem_service.service")"
require_file "$(path_join "$SYSTEMD_DIR" "linqu_mem_service.host.service")"
require_executable "$(path_join "$SCRIPT_INSTALL_DIR" "verify_mem_service_installed_layout.sh")"
require_executable "$(path_join "$SCRIPT_INSTALL_DIR" "verify_mem_service_release_certification.sh")"
require_executable "$(path_join "$SCRIPT_INSTALL_DIR" "verify_mem_service_linux_ops_evidence.sh")"
require_executable "$(path_join "$SCRIPT_INSTALL_DIR" "verify_mem_service_remote_transport_evidence.sh")"
require_executable "$(path_join "$SCRIPT_INSTALL_DIR" "run_mem_service_release_certification_ci.sh")"

PACKAGE_MANIFEST=$(path_join "$DATA_DIR" "package-manifest.txt")
RELEASE_MANIFEST=$(path_join "$DATA_DIR" "release-manifest.txt")
require_grep "$PACKAGE_MANIFEST" '^package_format=installed-layout-v1$'
require_grep "$PACKAGE_MANIFEST" '^file_class=release_scripts count=9$'
require_grep "$PACKAGE_MANIFEST" '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$'
require_grep "$PACKAGE_MANIFEST" '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$'
require_grep "$RELEASE_MANIFEST" '^release_script=share/lingqu/mem_service/scripts/verify_mem_service_installed_layout.sh$'
require_grep "$RELEASE_MANIFEST" '^release_script=share/lingqu/mem_service/scripts/run_mem_service_release_certification_ci.sh$'
require_grep "$(path_join "$SYSTEM_CONFIG_DIR" "mem_service.conf")" '^backend=snapshot+journal$'
require_grep "$(path_join "$SYSTEM_CONFIG_DIR" "mem_service.host.conf")" '^metrics_listen=tcp:127[.]0[.]0[.]1:9901$'

run_host_fixture version-fixtures
run_host_fixture package-fixtures
run_host_fixture release-fixtures

if [ "$DRY_RUN" -eq 0 ]; then
  printf '[mem-service-installed-layout] PASS root=%s prefix=%s host=%s\n' \
    "$INSTALL_ROOT" "$INSTALL_PREFIX" "$HOST_BIN"
fi
