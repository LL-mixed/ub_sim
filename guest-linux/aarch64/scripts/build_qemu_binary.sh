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
    while IFS= read -r file; do
      file_signature "$file"
    done |
    sort
}

apply_host_qemu_configure_args() {
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
