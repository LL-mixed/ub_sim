#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

source "$SCRIPT_DIR/qemu_ub_common.sh"

SRC_DIR="$(qemu_ub_source_path "$REPO_ROOT")"
BUILD_DIR="$(qemu_ub_build_path "$REPO_ROOT")"
BIN="$(qemu_ub_bin_path "$REPO_ROOT")"
TARGET_LIST="${QEMU_TARGET_LIST:-aarch64-softmmu}"
JOBS="${QEMU_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
CONFIGURE_ARGS="${QEMU_CONFIGURE_ARGS:---disable-werror}"
RECONFIGURE="${RECONFIGURE:-0}"
STAMP_FILE="$BUILD_DIR/.qemu_build.stamp"
SIM_QEMU_STATICLIB="${SIM_QEMU_STATICLIB:-}"
BUILD_HOST_OS="$(uname -s 2>/dev/null || echo unknown)"
STAT_BIN="${STAT_BIN:-$(command -v stat 2>/dev/null || echo stat)}"

print_qemu_build_deps_help() {
  cat >&2 <<'EOF'
[build_qemu_binary] missing native QEMU build dependencies.
[build_qemu_binary] container helper:
[build_qemu_binary]   ./guest-linux/aarch64/scripts/prepare_w5_container_deps.sh
[build_qemu_binary] openEuler/Fedora/RHEL container, current python:
[build_qemu_binary]   dnf install -y glib2-devel pixman-devel zlib-devel pkgconf-pkg-config ninja-build gcc gcc-c++ make python3-pip
[build_qemu_binary]   python3 -m pip install distlib
[build_qemu_binary] openEuler/Fedora/RHEL container, system python:
[build_qemu_binary]   dnf install -y python3-distlib glib2-devel pixman-devel zlib-devel pkgconf-pkg-config ninja-build gcc gcc-c++ make python3-pip
[build_qemu_binary]   export QEMU_CONFIGURE_ARGS="--disable-werror --python=/usr/bin/python3"
[build_qemu_binary] Debian/Ubuntu container:
[build_qemu_binary]   apt-get update && apt-get install -y python3-distlib libglib2.0-dev libpixman-1-dev zlib1g-dev pkg-config ninja-build gcc g++ make python3-pip
EOF
}

check_python_distlib() {
  local python_bin="$1"
  "$python_bin" - <<'PY' >/dev/null 2>&1
try:
    import distlib.scripts
    import distlib.version
except ImportError:
    from pip._vendor import distlib
    import pip._vendor.distlib.scripts
    import pip._vendor.distlib.version
PY
}

qemu_configure_python_bin() {
  local arg

  if [[ -n "${PYTHON:-}" ]]; then
    echo "$PYTHON"
    return
  fi
  for arg in ${(z)CONFIGURE_ARGS}; do
    case "$arg" in
      --python=*)
        echo "${arg#--python=}"
        return
        ;;
    esac
  done
  echo python3
}

check_qemu_build_host_deps() {
  local missing=()
  local python_bin
  local pkg

  python_bin="$(qemu_configure_python_bin)"
  if ! command -v "$python_bin" >/dev/null 2>&1; then
    missing+=("$python_bin")
  fi
  for tool in pkg-config ninja gcc make; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      missing+=("$tool")
    fi
  done
  if command -v "$python_bin" >/dev/null 2>&1 && ! check_python_distlib "$python_bin"; then
    missing+=("$python_bin distlib")
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    for pkg in glib-2.0 pixman-1 zlib; do
      if ! pkg-config --exists "$pkg"; then
        missing+=("pkg-config:$pkg")
      fi
    done
  fi

  if (( ${#missing[@]} > 0 )); then
    printf '[build_qemu_binary] missing: %s\n' "${(j:, :)missing}" >&2
    print_qemu_build_deps_help
    exit 1
  fi
}

file_signature() {
  local path="$1"

  case "$BUILD_HOST_OS" in
    Darwin|FreeBSD)
      "$STAT_BIN" -f '%N:%m:%z' "$path"
      ;;
    *)
      "$STAT_BIN" -c '%n:%Y:%s' "$path" 2>/dev/null || "$STAT_BIN" -f '%N:%m:%z' "$path"
      ;;
  esac
}

file_mtime() {
  local path="$1"

  case "$BUILD_HOST_OS" in
    Darwin|FreeBSD)
      "$STAT_BIN" -f '%m' "$path"
      ;;
    *)
      "$STAT_BIN" -c '%Y' "$path" 2>/dev/null || "$STAT_BIN" -f '%m' "$path"
      ;;
  esac
}

qemu_source_signature() {
  local file

  find "$SRC_DIR/hw/ub" "$SRC_DIR/include/hw/ub" -type f \
    \( -name '*.c' -o -name '*.h' -o -name 'meson.build' \) -print 2>/dev/null |
    {
      cat
      printf '%s\n' \
        "$SRC_DIR/hw/arm/virt.c" \
        "$SRC_DIR/include/hw/arm/virt.h" \
        "$SRC_DIR/target/arm/tcg/tlb_helper.c"
    } |
    while IFS= read -r file; do
      [[ -f "$file" ]] || continue
      file_signature "$file"
    done |
    sort
}

apply_host_qemu_configure_args() {
  if [[ "$CONFIGURE_ARGS" != *"--disable-docs"* && "$CONFIGURE_ARGS" != *"--enable-docs"* ]]; then
    CONFIGURE_ARGS="${CONFIGURE_ARGS} --disable-docs"
  fi
  case "$BUILD_HOST_OS" in
    Darwin)
      if [[ "$CONFIGURE_ARGS" != *"--disable-zstd"* && "$CONFIGURE_ARGS" != *"--enable-zstd"* ]]; then
        CONFIGURE_ARGS="${CONFIGURE_ARGS} --disable-zstd"
        echo "[build_qemu_binary] macOS build host detected; adding --disable-zstd" >&2
      fi
      ;;
  esac
}

find_sim_qemu_staticlib() {
  local candidate
  if [[ -n "$SIM_QEMU_STATICLIB" ]]; then
    echo "$SIM_QEMU_STATICLIB"
    return 0
  fi
  for candidate in \
    "$REPO_ROOT/target/release/libsim_qemu.a" \
    "$REPO_ROOT"/target/*/release/libsim_qemu.a(N); do
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  done
  echo "$REPO_ROOT/target/release/libsim_qemu.a"
}

build_sim_qemu_staticlib() {
  (
    cd "$REPO_ROOT"
    cargo build --release -p sim-qemu
  )
}

ensure_sim_qemu_link_args() {
  build_sim_qemu_staticlib
  SIM_QEMU_STATICLIB="$(find_sim_qemu_staticlib)"
  if [[ ! -f "$SIM_QEMU_STATICLIB" ]]; then
    echo "[build_qemu_binary] error: missing sim-qemu staticlib: $SIM_QEMU_STATICLIB" >&2
    exit 1
  fi
  if [[ "$CONFIGURE_ARGS" != *"$SIM_QEMU_STATICLIB"* ]]; then
    CONFIGURE_ARGS="${CONFIGURE_ARGS} --extra-ldflags=$SIM_QEMU_STATICLIB"
  fi
}

qemu_build_signature() {
  local qemu_head=""
  local qemu_src_sig=""
  local rust_lib_sig=""
  qemu_head="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo "")"
  qemu_src_sig="$(qemu_source_signature || true)"
  if [[ -f "$SIM_QEMU_STATICLIB" ]]; then
    rust_lib_sig="$(file_signature "$SIM_QEMU_STATICLIB" 2>/dev/null || echo "")"
  fi
  printf 'qemu_head=%s\nqemu_src_sig=%s\ntarget_list=%s\nconfigure_args=%s\nsim_qemu_staticlib=%s\n' \
    "$qemu_head" "$qemu_src_sig" "$TARGET_LIST" "$CONFIGURE_ARGS" "$rust_lib_sig"
}

qemu_build_stamp_matches() {
  [[ -f "$STAMP_FILE" ]] || return 1
  [[ "$(cat "$STAMP_FILE" 2>/dev/null)" == "$(qemu_build_signature)" ]]
}

staticlib_newer_than_qemu_binary() {
  local lib_mtime=""
  local bin_mtime=""

  [[ -n "$SIM_QEMU_STATICLIB" && -f "$SIM_QEMU_STATICLIB" && -e "$BIN" ]] || return 1
  lib_mtime="$(file_mtime "$SIM_QEMU_STATICLIB" 2>/dev/null || echo 0)"
  bin_mtime="$(file_mtime "$BIN" 2>/dev/null || echo 0)"
  [[ "$lib_mtime" == <-> && "$bin_mtime" == <-> ]] || return 1
  (( lib_mtime > bin_mtime ))
}

write_qemu_build_stamp() {
  qemu_build_signature > "$STAMP_FILE"
}

if [[ ! -d "$SRC_DIR" ]]; then
  echo "[build_qemu_binary] error: missing QEMU source dir: $SRC_DIR" >&2
  exit 1
fi

check_qemu_build_host_deps
apply_host_qemu_configure_args
ensure_sim_qemu_link_args
mkdir -p "$BUILD_DIR"

if [[ "$RECONFIGURE" != "1" && -x "$BIN" ]] &&
   qemu_build_stamp_matches &&
   ! staticlib_newer_than_qemu_binary &&
   qemu_ub_supports_required_opts "$BIN"; then
  echo "[build_qemu_binary] using existing QEMU binary: $BIN" >&2
  echo "$BIN"
  exit 0
fi

if [[ ! -f "$BUILD_DIR/build.ninja" || "$RECONFIGURE" == "1" ]] || ! qemu_build_stamp_matches; then
  echo "[build_qemu_binary] configuring QEMU in $BUILD_DIR" >&2
  (
    cd "$BUILD_DIR"
    "$SRC_DIR/configure" --target-list="$TARGET_LIST" ${=CONFIGURE_ARGS}
  )
fi

if staticlib_newer_than_qemu_binary; then
  echo "[build_qemu_binary] QEMU binary is older than sim-qemu staticlib; forcing relink" >&2
  rm -f "$BIN"
fi

echo "[build_qemu_binary] building qemu-system-aarch64" >&2
(
  cd "$BUILD_DIR"
  ninja -j"$JOBS" qemu-system-aarch64
)

if [[ ! -x "$BIN" ]]; then
  echo "[build_qemu_binary] error: missing binary after build: $BIN" >&2
  exit 1
fi

if ! qemu_ub_supports_required_opts "$BIN"; then
  echo "[build_qemu_binary] error: built binary missing required UB options: $BIN" >&2
  exit 1
fi

write_qemu_build_stamp

echo "$BIN"
