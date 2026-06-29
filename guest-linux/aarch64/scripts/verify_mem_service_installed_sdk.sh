#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_ROOT=""
INSTALL_PREFIX=""
WORK_DIR=""
CC_CMD=${CC:-cc}
PKG_CONFIG_CMD=${PKG_CONFIG:-pkg-config}
DRY_RUN=0
NO_RUNTIME=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_installed_sdk.sh [--root DIR] [--prefix DIR] [--work-dir DIR] [--no-runtime] [--dry-run]

Verifies the installed mem_service SDK without requiring a source checkout.

Options:
  --root DIR        Installed root, useful for DESTDIR validation. Defaults to
                    deriving the root from this script location.
  --prefix DIR      Installed prefix inside --root. Defaults to deriving the
                    prefix from this script location.
  --work-dir DIR    Build/runtime scratch directory.
  --cc CMD          C compiler used to compile installed SDK examples.
  --pkg-config CMD  pkg-config command used to discover installed SDK metadata.
  --no-runtime      Compile the external clients but do not start the daemon.
  --dry-run         Print the checks without running them.
  -h, --help        Show this help.
EOF
}

fail() {
  echo "[mem-service-installed-sdk] FAIL: $*" >&2
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

  if [ -z "$WORK_DIR" ]; then
    WORK_DIR="${TMPDIR:-/tmp}/linqu-mem-service-installed-sdk"
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

require_command() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'command -v %s\n' "$1"
    return
  fi
  command -v "$1" >/dev/null || fail "missing command: $1"
}

compile_example() {
  example_source=$1
  output_binary=$2
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'PKG_CONFIG_PATH=%s %s --define-prefix --cflags lingqu-mem-service\n' "$PKGCONFIG_DIR" "$PKG_CONFIG_CMD"
    printf 'PKG_CONFIG_PATH=%s %s --define-prefix --variable=sdk_sources lingqu-mem-service\n' "$PKGCONFIG_DIR" "$PKG_CONFIG_CMD"
    printf '%s $cflags %s $sdk_sources -o %s\n' "$CC_CMD" "$example_source" "$output_binary"
    return
  fi
  PKG_CONFIG_PATH=$PKGCONFIG_DIR
  export PKG_CONFIG_PATH
  cflags=$($PKG_CONFIG_CMD --define-prefix --cflags lingqu-mem-service)
  sdk_sources=$($PKG_CONFIG_CMD --define-prefix --variable=sdk_sources lingqu-mem-service)
  [ -n "$cflags" ] || fail "pkg-config returned empty Cflags"
  [ -n "$sdk_sources" ] || fail "pkg-config returned empty sdk_sources"
  # shellcheck disable=SC2086
  $CC_CMD $cflags "$example_source" $sdk_sources -o "$output_binary"
}

run_runtime() {
  if [ "$NO_RUNTIME" -eq 1 ]; then
    return
  fi
  if [ "$DRY_RUN" -eq 1 ]; then
    printf '%s serve --listen unix:%s/mem_service.sock --store %s/store.snapshot\n' "$HOST_BIN" "$WORK_DIR" "$WORK_DIR"
    printf '%s/mem_service_serving_example unix:%s/mem_service.sock\n' "$WORK_DIR" "$WORK_DIR"
    printf '%s/mem_service_pretraining_example unix:%s/mem_service.sock\n' "$WORK_DIR" "$WORK_DIR"
    return
  fi

  socket=$WORK_DIR/mem_service.sock
  store=$WORK_DIR/store.snapshot
  rm -f "$socket" "$store" "$store.journal"
  "$HOST_BIN" serve --listen "unix:$socket" --store "$store" > "$WORK_DIR/daemon.stdout" 2> "$WORK_DIR/daemon.stderr" &
  pid=$!
  trap 'kill "$pid" >/dev/null 2>&1 || true; wait "$pid" >/dev/null 2>&1 || true' EXIT INT TERM

  i=0
  while ! "$HOST_BIN" ready --connect "unix:$socket" >/dev/null 2>&1; do
    if ! kill -0 "$pid" >/dev/null 2>&1; then
      cat "$WORK_DIR/daemon.stderr" >&2
      fail "installed daemon exited before ready"
    fi
    i=$((i + 1))
    if [ "$i" -ge 100 ]; then
      cat "$WORK_DIR/daemon.stderr" >&2
      fail "installed daemon did not become ready"
    fi
    sleep 0.05
  done

  "$WORK_DIR/mem_service_serving_example" "unix:$socket"
  "$WORK_DIR/mem_service_pretraining_example" "unix:$socket"
  "$HOST_BIN" status --connect "unix:$socket" >/dev/null
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
    --work-dir)
      [ "$#" -ge 2 ] || fail "--work-dir requires a path"
      WORK_DIR=$2
      shift 2
      ;;
    --cc)
      [ "$#" -ge 2 ] || fail "--cc requires a command"
      CC_CMD=$2
      shift 2
      ;;
    --pkg-config)
      [ "$#" -ge 2 ] || fail "--pkg-config requires a command"
      PKG_CONFIG_CMD=$2
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
EXAMPLE_DIR=$(path_join "$DATA_DIR" "examples")
PKGCONFIG_DIR=$(path_join "$INSTALL_PREFIX" "lib/pkgconfig")
PKGCONFIG_FILE=$(path_join "$PKGCONFIG_DIR" "lingqu-mem-service.pc")
HOST_BIN=$(path_join "$INSTALL_PREFIX" "libexec/lingqu/mem_service/linqu_mem_service_host")
SERVING_EXAMPLE=$(path_join "$EXAMPLE_DIR" "mem_service_serving_example.c")
PRETRAINING_EXAMPLE=$(path_join "$EXAMPLE_DIR" "mem_service_pretraining_example.c")

require_command "$CC_CMD"
require_command "$PKG_CONFIG_CMD"
require_file "$PKGCONFIG_FILE"
require_file "$SERVING_EXAMPLE"
require_file "$PRETRAINING_EXAMPLE"
require_executable "$HOST_BIN"

if [ "$DRY_RUN" -eq 0 ]; then
  rm -rf "$WORK_DIR"
  mkdir -p "$WORK_DIR"
  PKG_CONFIG_PATH=$PKGCONFIG_DIR "$PKG_CONFIG_CMD" --define-prefix --exists lingqu-mem-service ||
    fail "pkg-config cannot resolve lingqu-mem-service"
else
  printf 'PKG_CONFIG_PATH=%s %s --define-prefix --exists lingqu-mem-service\n' "$PKGCONFIG_DIR" "$PKG_CONFIG_CMD"
fi

compile_example "$SERVING_EXAMPLE" "$WORK_DIR/mem_service_serving_example"
compile_example "$PRETRAINING_EXAMPLE" "$WORK_DIR/mem_service_pretraining_example"
require_executable "$WORK_DIR/mem_service_serving_example"
require_executable "$WORK_DIR/mem_service_pretraining_example"
run_runtime

if [ "$DRY_RUN" -eq 0 ]; then
  printf '[mem-service-installed-sdk] PASS root=%s prefix=%s work_dir=%s\n' \
    "$INSTALL_ROOT" "$INSTALL_PREFIX" "$WORK_DIR"
fi
