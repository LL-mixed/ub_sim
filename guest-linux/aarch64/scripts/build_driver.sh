#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DRIVER_DIR="$ROOT_DIR/driver"
OUT_DIR="$ROOT_DIR/out/driver"

: "${KERNEL_BUILD_DIR:=}"
: "${CROSS_COMPILE:=aarch64-unknown-linux-gnu-}"
: "${ARCH:=arm64}"

if [[ -z "$KERNEL_BUILD_DIR" ]]; then
  if [[ -d "$ROOT_DIR/out/kernel_build" ]]; then
    KERNEL_BUILD_DIR="$ROOT_DIR/out/kernel_build"
  elif [[ -d "$ROOT_DIR/kernel_build" ]]; then
    # Legacy location (before moving build trees under out/)
    KERNEL_BUILD_DIR="$ROOT_DIR/kernel_build"
  else
    echo "KERNEL_BUILD_DIR is required" >&2
    echo "example: export KERNEL_BUILD_DIR=$ROOT_DIR/out/kernel_build" >&2
    exit 1
  fi
fi

mkdir -p "$OUT_DIR"

make -C "$KERNEL_BUILD_DIR" \
  M="$DRIVER_DIR" \
  O="$KERNEL_BUILD_DIR" \
  ARCH="$ARCH" \
  CROSS_COMPILE="$CROSS_COMPILE" \
  modules

cp "$DRIVER_DIR"/linqu_ub_drv.ko "$OUT_DIR"/
echo "$OUT_DIR/linqu_ub_drv.ko"
