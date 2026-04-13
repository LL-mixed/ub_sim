#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
MODULES_DIR="${MODULES_DIR:-$OUT_DIR/modules}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

ARTIFACT_SOURCE="${ARTIFACT_SOURCE:-auto}"   # auto|vm|local|none
SYNC_ARTIFACTS="${SYNC_ARTIFACTS:-1}"
BUILD_IN_VM="${BUILD_IN_VM:-1}"
BUILD_LINQU_DRIVER_IN_VM="${BUILD_LINQU_DRIVER_IN_VM:-1}"
LOCAL_KERNEL_IMAGE="${LOCAL_KERNEL_IMAGE:-}"
LOCAL_MODULES_DIR="${LOCAL_MODULES_DIR:-}"
CC="$(detect_aarch64_linux_cc)"
BUSYBOX_BIN="${BUSYBOX:-}"
if [[ -z "$CC" ]]; then
  echo "[build_guest_artifacts] error: AARCH64_LINUX_CC is required" >&2
  exit 1
fi

if [[ -z "$BUSYBOX_BIN" && -x "$ROOT_DIR/busybox-aarch64" ]]; then
  BUSYBOX_BIN="$ROOT_DIR/busybox-aarch64"
fi

if [[ -z "$BUSYBOX_BIN" || ! -x "$BUSYBOX_BIN" ]]; then
  BUSYBOX_BIN="$("$SCRIPT_DIR/prepare_busybox.sh")"
fi

ensure_dirs() {
  mkdir -p "$OUT_DIR" "$MODULES_DIR"
}

reset_module_artifacts() {
  ensure_dirs
  rm -f "$MODULES_DIR"/*.ko(N)
  rm -f "$OUT_DIR"/*.ko(N)
}

have_default_artifacts() {
  [[ -f "$OUT_DIR/Image" && -f "$OUT_DIR/initramfs.cpio.gz" ]]
}

import_local_artifacts() {
  if [[ -z "$LOCAL_KERNEL_IMAGE" || -z "$LOCAL_MODULES_DIR" ]]; then
    echo "[build_guest_artifacts] error: local mode requires LOCAL_KERNEL_IMAGE and LOCAL_MODULES_DIR" >&2
    return 1
  fi
  if [[ ! -f "$LOCAL_KERNEL_IMAGE" ]]; then
    echo "[build_guest_artifacts] error: local kernel image not found: $LOCAL_KERNEL_IMAGE" >&2
    return 1
  fi
  if [[ ! -d "$LOCAL_MODULES_DIR" ]]; then
    echo "[build_guest_artifacts] error: local modules dir not found: $LOCAL_MODULES_DIR" >&2
    return 1
  fi
  ensure_dirs
  reset_module_artifacts
  cp "$LOCAL_KERNEL_IMAGE" "$OUT_DIR/Image"
  local mod=""
  for mod in "$LOCAL_MODULES_DIR"/*.ko; do
    [[ -f "$mod" ]] || continue
    cp "$mod" "$MODULES_DIR/"
  done
}

vm_host_reachable() {
  local host="${VM_HOST:-ll@192.168.64.3}"
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" true >/dev/null 2>&1
}

sync_from_vm() {
  echo "[build_guest_artifacts] syncing guest kernel artifacts from VM" >&2
  (
    cd "$ROOT_DIR"
    reset_module_artifacts
    BUILD_IN_VM="$BUILD_IN_VM" \
    BUILD_LINQU_DRIVER_IN_VM="$BUILD_LINQU_DRIVER_IN_VM" \
    ./scripts/sync_ub_kernel_artifacts_from_vm.sh
  )
}

print_build_guest_help() {
  cat >&2 <<EOF
[build_guest_artifacts] no usable guest artifact source found
[build_guest_artifacts] supported modes:
[build_guest_artifacts]   ARTIFACT_SOURCE=auto  : reuse out/, then local import, then VM if reachable
[build_guest_artifacts]   ARTIFACT_SOURCE=local : require LOCAL_KERNEL_IMAGE + LOCAL_MODULES_DIR
[build_guest_artifacts]   ARTIFACT_SOURCE=vm    : force ./scripts/sync_ub_kernel_artifacts_from_vm.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=none  : only rebuild initramfs from existing out/
[build_guest_artifacts] busybox:
[build_guest_artifacts]   ./scripts/prepare_busybox.sh
[build_guest_artifacts] examples:
[build_guest_artifacts]   AARCH64_LINUX_CC=$CC BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=local LOCAL_KERNEL_IMAGE=/path/to/Image LOCAL_MODULES_DIR=/path/to/modules AARCH64_LINUX_CC=$CC ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=vm VM_HOST=\${VM_HOST:-ll@192.168.64.3} AARCH64_LINUX_CC=$CC ./scripts/build_guest_artifacts.sh
EOF
}

case "$ARTIFACT_SOURCE" in
  auto)
    if have_default_artifacts; then
      echo "[build_guest_artifacts] using existing local out/ artifacts" >&2
    elif [[ -n "$LOCAL_KERNEL_IMAGE" || -n "$LOCAL_MODULES_DIR" ]]; then
      echo "[build_guest_artifacts] importing guest artifacts from local paths" >&2
      import_local_artifacts
    elif [[ "$SYNC_ARTIFACTS" == "1" ]] && vm_host_reachable; then
      sync_from_vm
    else
      print_build_guest_help
      exit 1
    fi
    ;;
  local)
    echo "[build_guest_artifacts] importing guest artifacts from local paths" >&2
    import_local_artifacts
    ;;
  vm)
    sync_from_vm
    ;;
  none)
    if ! have_default_artifacts; then
      echo "[build_guest_artifacts] error: ARTIFACT_SOURCE=none requires existing out/Image and out/initramfs.cpio.gz" >&2
      print_build_guest_help
      exit 1
    fi
    ;;
  *)
    echo "[build_guest_artifacts] error: unsupported ARTIFACT_SOURCE=$ARTIFACT_SOURCE" >&2
    print_build_guest_help
    exit 1
    ;;
esac

echo "[build_guest_artifacts] rebuilding initramfs" >&2
(
  cd "$ROOT_DIR"
  AARCH64_LINUX_CC="$CC" BUSYBOX="$BUSYBOX_BIN" ./scripts/build_initramfs.sh
)
