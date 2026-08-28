#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DRIVER_DIR="$ROOT_DIR/driver"
OUT_DIR="$ROOT_DIR/out/driver"
KERNEL_SRC_DIR="${KERNEL_SRC_DIR:-$ROOT_DIR/../kernel_ub}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

: "${KERNEL_BUILD_DIR:=}"
: "${CROSS_COMPILE:=}"
: "${ARCH:=arm64}"

if [[ -d "$KERNEL_SRC_DIR" ]]; then
  KERNEL_SRC_DIR="$(cd "$KERNEL_SRC_DIR" && pwd)"
fi

if [[ -z "$CROSS_COMPILE" ]]; then
  CC="$(detect_aarch64_linux_cc)"
  if [[ -z "$CC" ]] || ! CROSS_COMPILE="$(aarch64_linux_cross_prefix "$CC")"; then
    echo "unable to detect an AArch64 Linux compiler" >&2
    exit 1
  fi
fi

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

if [[ ! -f "$KERNEL_BUILD_DIR/vmlinux.symvers" ]]; then
  echo "kernel build is missing vmlinux.symvers: $KERNEL_BUILD_DIR" >&2
  exit 1
fi
run_gnu_make -C "$KERNEL_SRC_DIR" O="$KERNEL_BUILD_DIR" \
  ARCH="$ARCH" CROSS_COMPILE="$CROSS_COMPILE" modules_prepare
cp "$KERNEL_BUILD_DIR/vmlinux.symvers" "$KERNEL_BUILD_DIR/Module.symvers"

run_gnu_make -C "$KERNEL_SRC_DIR" \
  O="$KERNEL_BUILD_DIR" \
  M="$DRIVER_DIR" \
  ARCH="$ARCH" \
  CROSS_COMPILE="$CROSS_COMPILE" \
  modules

cp "$DRIVER_DIR"/linqu_ub_drv.ko "$OUT_DIR"/
echo "$OUT_DIR/linqu_ub_drv.ko"
