#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

source "$SCRIPT_DIR/qemu_ub_common.sh"

SYNC_ARTIFACTS="${SYNC_ARTIFACTS:-1}"
BUILD_IN_VM="${BUILD_IN_VM:-1}"
BUILD_LINQU_DRIVER_IN_VM="${BUILD_LINQU_DRIVER_IN_VM:-1}"
CC="$(detect_aarch64_linux_cc)"
BUSYBOX_BIN="${BUSYBOX:-}"

if [[ -z "$CC" ]]; then
  echo "[build_guest_artifacts] error: AARCH64_LINUX_CC is required" >&2
  exit 1
fi

if [[ -z "$BUSYBOX_BIN" && -x "$ROOT_DIR/busybox-aarch64" ]]; then
  BUSYBOX_BIN="$ROOT_DIR/busybox-aarch64"
fi

if [[ "$SYNC_ARTIFACTS" == "1" ]]; then
  echo "[build_guest_artifacts] syncing guest kernel artifacts from VM" >&2
  (
    cd "$ROOT_DIR"
    BUILD_IN_VM="$BUILD_IN_VM" \
    BUILD_LINQU_DRIVER_IN_VM="$BUILD_LINQU_DRIVER_IN_VM" \
    ./scripts/sync_ub_kernel_artifacts_from_vm.sh
  )
fi

echo "[build_guest_artifacts] rebuilding initramfs" >&2
(
  cd "$ROOT_DIR"
  AARCH64_LINUX_CC="$CC" BUSYBOX="$BUSYBOX_BIN" ./scripts/build_initramfs.sh
)
