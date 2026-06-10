#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
MODULES_DIR="${MODULES_DIR:-$OUT_DIR/modules}"
KERNEL_STAMP_FILE="$OUT_DIR/.kernel_image.kernel_ub_head"
KERNEL_SRC_DIR="$ROOT_DIR/../kernel_ub"
KERNEL_BUILD_DIR="${KERNEL_BUILD_DIR:-$OUT_DIR/kernel_build}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

ARTIFACT_SOURCE="${ARTIFACT_SOURCE:-auto}"   # auto|native|remote|local|none
SYNC_ARTIFACTS="${SYNC_ARTIFACTS:-1}"
BUILD_ON_REMOTE="${BUILD_ON_REMOTE:-0}"
BUILD_LINQU_DRIVER_ON_REMOTE="${BUILD_LINQU_DRIVER_ON_REMOTE:-0}"
SYNC_KERNEL_SRC_TO_REMOTE="${SYNC_KERNEL_SRC_TO_REMOTE:-0}"
ALLOW_REMOTE_LINUX_ARTIFACTS="${ALLOW_REMOTE_LINUX_ARTIFACTS:-0}"
REMOTE_TMPDIR="${REMOTE_TMPDIR:-}"
LOCAL_KERNEL_IMAGE="${LOCAL_KERNEL_IMAGE:-}"
LOCAL_MODULES_DIR="${LOCAL_MODULES_DIR:-}"
KERNEL_DEFCONFIG="${KERNEL_DEFCONFIG:-openeuler_defconfig}"
KERNEL_ARCH="${KERNEL_ARCH:-arm64}"
KERNEL_JOBS="${KERNEL_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
CC="$(detect_aarch64_linux_cc)"
BUSYBOX_BIN="${BUSYBOX:-}"
if [[ -z "$CC" ]]; then
  echo "[build_guest_artifacts] error: AARCH64_LINUX_CC is required" >&2
  exit 1
fi
if [[ "$KERNEL_ARCH" != "arm64" ]]; then
  echo "[build_guest_artifacts] error: this guest target requires KERNEL_ARCH=arm64" >&2
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

current_kernel_submodule_head() {
  git -C "$KERNEL_SRC_DIR" rev-parse HEAD 2>/dev/null || echo ""
}

current_kernel_artifact_signature() {
  local current_head=""
  local tracked_path
  current_head="$(current_kernel_submodule_head)"
  [[ -n "$current_head" ]] || return 1

  printf 'kernel_head=%s\n' "$current_head"
  if git -C "$KERNEL_SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    for tracked_path in \
      drivers/ub/obmm \
      drivers/ub/ubus/ub_npu.c \
      drivers/ub/ubus/ub_ssd.c \
      drivers/ub/ubus/sim \
      include/uapi/asm-generic/mman-common.h \
      include/uapi/ub/ub_npu.h \
      include/uapi/ub/ub_ssd.h \
      include/uapi/ub/obmm.h \
      mm/mmap.c; do
      git -C "$KERNEL_SRC_DIR" status --porcelain --untracked-files=no -- "$tracked_path" || true
      git -C "$KERNEL_SRC_DIR" diff --binary -- "$tracked_path" || true
      git -C "$KERNEL_SRC_DIR" diff --cached --binary -- "$tracked_path" || true
    done
  fi
}

kernel_image_stamp_matches() {
  local current_signature=""
  local legacy_head=""
  local stamp=""
  current_signature="$(current_kernel_artifact_signature)" || return 1
  legacy_head="$(current_kernel_submodule_head)"
  [[ -f "$KERNEL_STAMP_FILE" ]] || return 1
  stamp="$(cat "$KERNEL_STAMP_FILE" 2>/dev/null)"
  [[ "$stamp" == "$current_signature" ]] && return 0
  [[ "$stamp" == "$legacy_head" && "$current_signature" == "kernel_head=$legacy_head" ]]
}

write_kernel_image_stamp() {
  current_kernel_artifact_signature > "$KERNEL_STAMP_FILE"
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
  for mod in "$LOCAL_MODULES_DIR"/*.ko(N); do
    [[ -f "$mod" ]] || continue
    cp "$mod" "$MODULES_DIR/"
  done
  write_kernel_image_stamp || true
}

host_is_linux() {
  [[ "$(uname -s 2>/dev/null || echo "")" == "Linux" ]]
}

cross_compile_prefix() {
  local cc_path="$1"
  local cc_name=""
  cc_name="$(basename "$cc_path")"
  if [[ "$cc_name" != *gcc ]]; then
    echo "[build_guest_artifacts] error: cannot derive CROSS_COMPILE from $cc_path" >&2
    return 1
  fi
  echo "${cc_name%gcc}"
}

native_build_available() {
  host_is_linux || return 1
  [[ -d "$KERNEL_SRC_DIR" ]] || return 1
  [[ -x "$KERNEL_SRC_DIR/scripts/config" ]] || return 1
  command -v make >/dev/null 2>&1 || return 1
  [[ -n "$CC" ]] || return 1
}

copy_module_if_present() {
  local src="$1"
  local dst_name="$2"
  if [[ -f "$src" ]]; then
    cp "$src" "$MODULES_DIR/$dst_name"
  fi
}

copy_kernel_module_if_enabled() {
  local config_key="$1"
  local src="$2"
  local dst_name="$3"

  if grep -q "^${config_key}=m$" "$KERNEL_BUILD_DIR/.config"; then
    copy_module_if_present "$src" "$dst_name"
  else
    rm -f "$MODULES_DIR/$dst_name"
  fi
}

copy_native_modules() {
  copy_kernel_module_if_enabled "CONFIG_UB_HISI_UBUS" "$KERNEL_BUILD_DIR/drivers/ub/ubus/vendor/hisilicon/hisi_ubus.ko" "hisi_ubus.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UBUS_BUS" "$KERNEL_BUILD_DIR/drivers/ub/ubus/ubus.ko" "ubus.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UBUS_SIM_DECODER" "$KERNEL_BUILD_DIR/drivers/ub/ubus/sim/ub-sim-decoder.ko" "ub-sim-decoder.ko"
  copy_kernel_module_if_enabled "CONFIG_OBMM" "$KERNEL_BUILD_DIR/drivers/ub/obmm/obmm.ko" "obmm.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UBASE" "$KERNEL_BUILD_DIR/drivers/ub/ubase/ubase.ko" "ubase.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_URMA" "$KERNEL_BUILD_DIR/drivers/ub/urma/ubcore/ubcore.ko" "ubcore.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UDMA" "$KERNEL_BUILD_DIR/drivers/ub/urma/hw/udma/udma.ko" "udma.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_URMA" "$KERNEL_BUILD_DIR/drivers/ub/urma/ulp/ipourma/ipourma.ko" "ipourma.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_URMA" "$KERNEL_BUILD_DIR/drivers/ub/urma/uburma/uburma.ko" "uburma.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UMMU_CORE_DRIVER" "$KERNEL_BUILD_DIR/drivers/iommu/hisilicon/ummu-core/ummu-core.ko" "ummu-core.ko"
  copy_kernel_module_if_enabled "CONFIG_UB_UMMU" "$KERNEL_BUILD_DIR/drivers/iommu/hisilicon/ummu.ko" "ummu.ko"
  copy_module_if_present "$ROOT_DIR/driver/linqu_ub_drv.ko" "linqu_ub_drv.ko"
}

configure_native_kernel() {
  local cross_prefix="$1"
  mkdir -p "$KERNEL_BUILD_DIR"
  make -C "$KERNEL_SRC_DIR" O="$KERNEL_BUILD_DIR" ARCH="$KERNEL_ARCH" CROSS_COMPILE="$cross_prefix" "$KERNEL_DEFCONFIG"
  "$KERNEL_SRC_DIR/scripts/config" --file "$KERNEL_BUILD_DIR/.config" \
    -e UB \
    -e UB_UBUS \
    -e UB_UBUS_BUS \
    -e UB_UBUS_USI \
    -e UB_HISI_UBUS \
    -e HISI_SOC_CACHE \
    -e OBMM \
    -e UB_UBUS_SIM_DECODER \
    -e IPV6 \
    -e UB_UBASE \
    -e UB_URMA \
    -e UB_UDMA \
    -e UB_UBFI \
    -e ARCH_HISI \
    -e UB_UMMU \
    -e UB_UMMU_CORE \
    -e UB_UMMU_CORE_DRIVER \
    -d DEBUG_INFO_BTF \
    -d PAHOLE_HAS_SPLIT_BTF
  make -C "$KERNEL_SRC_DIR" O="$KERNEL_BUILD_DIR" ARCH="$KERNEL_ARCH" CROSS_COMPILE="$cross_prefix" olddefconfig
}

build_native_artifacts() {
  local cross_prefix=""
  if ! native_build_available; then
    echo "[build_guest_artifacts] error: native build requires Linux, make, kernel_ub, scripts/config, and AARCH64_LINUX_CC" >&2
    return 1
  fi
  cross_prefix="$(cross_compile_prefix "$CC")"
  echo "[build_guest_artifacts] native cross build: kernel=$KERNEL_SRC_DIR build=$KERNEL_BUILD_DIR arch=$KERNEL_ARCH defconfig=$KERNEL_DEFCONFIG cc=$CC" >&2
  ensure_dirs
  reset_module_artifacts
  configure_native_kernel "$cross_prefix"
  make -C "$KERNEL_SRC_DIR" O="$KERNEL_BUILD_DIR" ARCH="$KERNEL_ARCH" CROSS_COMPILE="$cross_prefix" KALLSYMS_EXTRA_PASS=1 -j"$KERNEL_JOBS" Image modules
  if [[ -d "$ROOT_DIR/driver" && -f "$ROOT_DIR/driver/Makefile" ]]; then
    make -C "$KERNEL_BUILD_DIR" M="$ROOT_DIR/driver" O="$KERNEL_BUILD_DIR" ARCH="$KERNEL_ARCH" CROSS_COMPILE="$cross_prefix" modules
  fi
  cp "$KERNEL_BUILD_DIR/arch/arm64/boot/Image" "$OUT_DIR/Image"
  copy_native_modules
  write_kernel_image_stamp || true
}

remote_linux_host_reachable() {
  local host="${REMOTE_LINUX_HOST:-}"
  [[ "$ALLOW_REMOTE_LINUX_ARTIFACTS" == "1" && -n "$host" ]] || return 1
  ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" true >/dev/null 2>&1
}

sync_from_remote_linux() {
  if [[ "$ALLOW_REMOTE_LINUX_ARTIFACTS" != "1" || -z "${REMOTE_LINUX_HOST:-}" ]]; then
    echo "[build_guest_artifacts] error: remote Linux artifact sync requires ALLOW_REMOTE_LINUX_ARTIFACTS=1 and REMOTE_LINUX_HOST=<ssh-target>" >&2
    return 1
  fi
  echo "[build_guest_artifacts] syncing guest kernel artifacts from remote Linux" >&2
  (
    cd "$ROOT_DIR"
    reset_module_artifacts
    BUILD_ON_REMOTE="$BUILD_ON_REMOTE" \
    BUILD_LINQU_DRIVER_ON_REMOTE="$BUILD_LINQU_DRIVER_ON_REMOTE" \
    SYNC_KERNEL_SRC_TO_REMOTE="$SYNC_KERNEL_SRC_TO_REMOTE" \
    REMOTE_LINUX_HOST="${REMOTE_LINUX_HOST:-}" \
    REMOTE_KERNEL_SRC="${REMOTE_KERNEL_SRC:-}" \
    REMOTE_KERNEL_BUILD="${REMOTE_KERNEL_BUILD:-}" \
    REMOTE_TMPDIR="${REMOTE_TMPDIR}" \
    REMOTE_LINQU_DRIVER_DIR="${REMOTE_LINQU_DRIVER_DIR:-}" \
    REMOTE_LINQU_MODULE_PATH="${REMOTE_LINQU_MODULE_PATH:-}" \
    REMOTE_REUSE_KERNEL_CONFIG="${REMOTE_REUSE_KERNEL_CONFIG:-0}" \
    ./scripts/sync_ub_kernel_artifacts_from_remote_linux.sh
  )
  write_kernel_image_stamp || true
}

print_build_guest_help() {
  cat >&2 <<EOF
[build_guest_artifacts] no usable guest artifact source found
[build_guest_artifacts] supported modes:
[build_guest_artifacts]   ARTIFACT_SOURCE=auto   : reuse out/, local import, then native Linux cross build; never contacts a remote Linux host
[build_guest_artifacts]   ARTIFACT_SOURCE=native : build arm64 kernel/modules locally on Linux with KERNEL_DEFCONFIG=openeuler_defconfig and AARCH64_LINUX_CC
[build_guest_artifacts]   ARTIFACT_SOURCE=local : require LOCAL_KERNEL_IMAGE + LOCAL_MODULES_DIR
[build_guest_artifacts]   ARTIFACT_SOURCE=remote: requires ALLOW_REMOTE_LINUX_ARTIFACTS=1 and REMOTE_LINUX_HOST=<ssh-target>
[build_guest_artifacts]   ARTIFACT_SOURCE=none  : only rebuild initramfs from existing out/
[build_guest_artifacts] busybox:
[build_guest_artifacts]   ./scripts/prepare_busybox.sh
[build_guest_artifacts] examples:
[build_guest_artifacts]   AARCH64_LINUX_CC=$CC BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=native AARCH64_LINUX_CC=$CC BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=local LOCAL_KERNEL_IMAGE=/path/to/Image LOCAL_MODULES_DIR=/path/to/modules AARCH64_LINUX_CC=$CC ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=remote ALLOW_REMOTE_LINUX_ARTIFACTS=1 REMOTE_LINUX_HOST=user@build-host REMOTE_KERNEL_SRC=/path/to/kernel_ub REMOTE_KERNEL_BUILD=/path/to/kernel_build AARCH64_LINUX_CC=$CC ./scripts/build_guest_artifacts.sh
[build_guest_artifacts]   ARTIFACT_SOURCE=remote ALLOW_REMOTE_LINUX_ARTIFACTS=1 REMOTE_LINUX_HOST=user@build-host REMOTE_TMPDIR=/mnt/share/tmp REMOTE_KERNEL_SRC=/mnt/share/... REMOTE_KERNEL_BUILD=/mnt/share/... AARCH64_LINUX_CC=$CC ./scripts/build_guest_artifacts.sh
EOF
}

case "$ARTIFACT_SOURCE" in
  auto)
    if have_default_artifacts && kernel_image_stamp_matches; then
      echo "[build_guest_artifacts] using existing local out/ artifacts" >&2
    elif have_default_artifacts; then
      echo "[build_guest_artifacts] existing Image/initramfs are stale for current kernel_ub source signature" >&2
      if [[ -n "$LOCAL_KERNEL_IMAGE" || -n "$LOCAL_MODULES_DIR" ]]; then
        echo "[build_guest_artifacts] importing refreshed guest artifacts from local paths" >&2
        import_local_artifacts
      elif native_build_available; then
        build_native_artifacts
      else
        print_build_guest_help
        exit 1
      fi
    elif [[ -n "$LOCAL_KERNEL_IMAGE" || -n "$LOCAL_MODULES_DIR" ]]; then
      echo "[build_guest_artifacts] importing guest artifacts from local paths" >&2
      import_local_artifacts
    elif native_build_available; then
      build_native_artifacts
    else
      print_build_guest_help
      exit 1
    fi
    ;;
  native)
    build_native_artifacts
    ;;
  local)
    echo "[build_guest_artifacts] importing guest artifacts from local paths" >&2
    import_local_artifacts
    ;;
  remote)
    sync_from_remote_linux
    ;;
  none)
    if ! have_default_artifacts; then
      echo "[build_guest_artifacts] error: ARTIFACT_SOURCE=none requires existing out/Image and out/initramfs.cpio.gz" >&2
      print_build_guest_help
      exit 1
    fi
    if ! kernel_image_stamp_matches; then
      echo "[build_guest_artifacts] error: ARTIFACT_SOURCE=none requires an Image matching current kernel_ub source signature" >&2
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
