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
CONFIGURE_ARGS="${QEMU_CONFIGURE_ARGS:-}"
RECONFIGURE="${RECONFIGURE:-0}"
STAMP_FILE="$BUILD_DIR/.qemu_build.stamp"
SIM_QEMU_STATICLIB="$REPO_ROOT/target/release/libsim_qemu.a"

build_sim_qemu_staticlib() {
  (
    cd "$REPO_ROOT"
    cargo build --release -p sim-qemu
  )
}

ensure_sim_qemu_link_args() {
  build_sim_qemu_staticlib
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
  qemu_src_sig="$(find "$SRC_DIR/hw/ub" "$SRC_DIR/include/hw/ub" -type f \
    \( -name '*.c' -o -name '*.h' -o -name 'meson.build' \) \
    -exec stat -f '%N:%m:%z' {} \; 2>/dev/null | sort || true)"
  if [[ -f "$SIM_QEMU_STATICLIB" ]]; then
    rust_lib_sig="$(stat -f '%m:%z' "$SIM_QEMU_STATICLIB" 2>/dev/null || stat -c '%Y:%s' "$SIM_QEMU_STATICLIB" 2>/dev/null || echo "")"
  fi
  printf 'qemu_head=%s\nqemu_src_sig=%s\ntarget_list=%s\nconfigure_args=%s\nsim_qemu_staticlib=%s\n' \
    "$qemu_head" "$qemu_src_sig" "$TARGET_LIST" "$CONFIGURE_ARGS" "$rust_lib_sig"
}

qemu_build_stamp_matches() {
  [[ -f "$STAMP_FILE" ]] || return 1
  [[ "$(cat "$STAMP_FILE" 2>/dev/null)" == "$(qemu_build_signature)" ]]
}

write_qemu_build_stamp() {
  qemu_build_signature > "$STAMP_FILE"
}

if [[ ! -d "$SRC_DIR" ]]; then
  echo "[build_qemu_binary] error: missing QEMU source dir: $SRC_DIR" >&2
  exit 1
fi

ensure_sim_qemu_link_args
mkdir -p "$BUILD_DIR"

if [[ "$RECONFIGURE" != "1" && -x "$BIN" && qemu_build_stamp_matches ]] && qemu_ub_supports_required_opts "$BIN"; then
  echo "[build_qemu_binary] using existing QEMU binary: $BIN" >&2
  echo "$BIN"
  exit 0
fi

if [[ ! -f "$BUILD_DIR/build.ninja" || "$RECONFIGURE" == "1" || ! qemu_build_stamp_matches ]]; then
  echo "[build_qemu_binary] configuring QEMU in $BUILD_DIR" >&2
  (
    cd "$BUILD_DIR"
    "$SRC_DIR/configure" --target-list="$TARGET_LIST" ${=CONFIGURE_ARGS}
  )
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
