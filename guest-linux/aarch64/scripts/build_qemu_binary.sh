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

qemu_build_signature() {
  local qemu_head=""
  qemu_head="$(git -C "$SRC_DIR" rev-parse HEAD 2>/dev/null || echo "")"
  printf 'qemu_head=%s\ntarget_list=%s\nconfigure_args=%s\n' \
    "$qemu_head" "$TARGET_LIST" "$CONFIGURE_ARGS"
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
