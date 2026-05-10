#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/out}"
MODULES_DIR="${MODULES_DIR:-$OUT_DIR/modules}"

REMOTE_LINUX_HOST="${REMOTE_LINUX_HOST:-}"
REMOTE_KERNEL_SRC="${REMOTE_KERNEL_SRC:-}"
REMOTE_KERNEL_BUILD="${REMOTE_KERNEL_BUILD:-}"
REMOTE_LINQU_DRIVER_DIR="${REMOTE_LINQU_DRIVER_DIR:-}"

REMOTE_IMAGE_PATH="${REMOTE_IMAGE_PATH:-$REMOTE_KERNEL_BUILD/arch/arm64/boot/Image}"
REMOTE_HISI_MODULE_PATH="${REMOTE_HISI_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/ubus/vendor/hisilicon/hisi_ubus.ko}"
REMOTE_UBUS_MODULE_PATH="${REMOTE_UBUS_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/ubus/ubus.ko}"
REMOTE_UB_SIM_DECODER_MODULE_PATH="${REMOTE_UB_SIM_DECODER_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/ubus/sim/ub-sim-decoder.ko}"
REMOTE_OBMM_MODULE_PATH="${REMOTE_OBMM_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/obmm/obmm.ko}"
REMOTE_UBASE_MODULE_PATH="${REMOTE_UBASE_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/ubase/ubase.ko}"
REMOTE_UBCORE_MODULE_PATH="${REMOTE_UBCORE_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/urma/ubcore/ubcore.ko}"
REMOTE_UDMA_MODULE_PATH="${REMOTE_UDMA_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/urma/hw/udma/udma.ko}"
REMOTE_IPOURMA_MODULE_PATH="${REMOTE_IPOURMA_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/urma/ulp/ipourma/ipourma.ko}"
REMOTE_UBURMA_MODULE_PATH="${REMOTE_UBURMA_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/ub/urma/uburma/uburma.ko}"
REMOTE_UMMU_CORE_MODULE_PATH="${REMOTE_UMMU_CORE_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/iommu/hisilicon/ummu-core/ummu-core.ko}"
REMOTE_UMMU_MODULE_PATH="${REMOTE_UMMU_MODULE_PATH:-$REMOTE_KERNEL_BUILD/drivers/iommu/hisilicon/ummu.ko}"
REMOTE_LINQU_MODULE_PATH="${REMOTE_LINQU_MODULE_PATH:-}"

BUILD_ON_REMOTE="${BUILD_ON_REMOTE:-0}"
BUILD_LINQU_DRIVER_ON_REMOTE="${BUILD_LINQU_DRIVER_ON_REMOTE:-0}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"
REMOTE_CROSS_COMPILE="${REMOTE_CROSS_COMPILE:-aarch64-linux-gnu-}"
REMOTE_ARCH="${REMOTE_ARCH:-arm64}"
REMOTE_KERNEL_DEFCONFIG="${REMOTE_KERNEL_DEFCONFIG:-openeuler_defconfig}"
REMOTE_REUSE_KERNEL_CONFIG="${REMOTE_REUSE_KERNEL_CONFIG:-0}"

if [[ -z "$REMOTE_LINUX_HOST" ]]; then
  echo "[sync] error: REMOTE_LINUX_HOST is required; this script never uses an implicit remote target" >&2
  exit 1
fi
if [[ -z "$REMOTE_KERNEL_SRC" || -z "$REMOTE_KERNEL_BUILD" ]]; then
  echo "[sync] error: REMOTE_KERNEL_SRC and REMOTE_KERNEL_BUILD are required" >&2
  exit 1
fi
if [[ "$REMOTE_ARCH" != "arm64" ]]; then
  echo "[sync] error: this guest target requires REMOTE_ARCH=arm64" >&2
  exit 1
fi

normalize_remote_sim_config() {
  ssh "$REMOTE_LINUX_HOST" "
    set -euo pipefail
    cd '$REMOTE_KERNEL_SRC'
    if [[ '$REMOTE_REUSE_KERNEL_CONFIG' == '1' ]]; then
      test -f '$REMOTE_KERNEL_BUILD/.config'
    else
      make O='$REMOTE_KERNEL_BUILD' ARCH='$REMOTE_ARCH' CROSS_COMPILE='$REMOTE_CROSS_COMPILE' '$REMOTE_KERNEL_DEFCONFIG'
    fi
    ./scripts/config --file '$REMOTE_KERNEL_BUILD/.config' \
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
      -e AUXILIARY_BUS \
      -e UB_UMMU \
      -e UB_UMMU_CORE \
      -e UB_UMMU_CORE_DRIVER \
      -d DEBUG_INFO_BTF \
      -d PAHOLE_HAS_SPLIT_BTF
    make O='$REMOTE_KERNEL_BUILD' ARCH='$REMOTE_ARCH' CROSS_COMPILE='$REMOTE_CROSS_COMPILE' olddefconfig
  "
}

sync_optional_module_from_config() {
  local config_key="$1"
  local remote_path="$2"
  local local_path="$3"

  if ssh "$REMOTE_LINUX_HOST" "grep -q '^${config_key}=m$' '$REMOTE_KERNEL_BUILD/.config'"; then
    copy_optional "$remote_path" "$local_path"
  else
    rm -f "$local_path"
  fi
}

copy_optional() {
  local remote_path="$1"
  local local_path="$2"
  if scp "$REMOTE_LINUX_HOST:$remote_path" "$local_path"; then
    :
  else
    echo "[sync] warn: failed to copy $remote_path" >&2
  fi
}

if [[ "$BUILD_ON_REMOTE" == "1" ]]; then
  normalize_remote_sim_config
  ssh "$REMOTE_LINUX_HOST" "
    set -euo pipefail
    cd '$REMOTE_KERNEL_SRC'
    make O='$REMOTE_KERNEL_BUILD' ARCH='$REMOTE_ARCH' CROSS_COMPILE='$REMOTE_CROSS_COMPILE' KALLSYMS_EXTRA_PASS=1 -j'$JOBS' Image modules
  "

  if [[ "$BUILD_LINQU_DRIVER_ON_REMOTE" == "1" ]]; then
    ssh "$REMOTE_LINUX_HOST" "
      set -euo pipefail
      if [[ -d '$REMOTE_LINQU_DRIVER_DIR' ]] && [[ -f '$REMOTE_LINQU_DRIVER_DIR/Makefile' ]]; then
        make -C '$REMOTE_KERNEL_BUILD' M='$REMOTE_LINQU_DRIVER_DIR' O='$REMOTE_KERNEL_BUILD' ARCH='$REMOTE_ARCH' CROSS_COMPILE='$REMOTE_CROSS_COMPILE' modules
      else
        echo '[sync] skip linqu_ub_drv.ko build on remote Linux: missing $REMOTE_LINQU_DRIVER_DIR or Makefile' >&2
      fi
    "
  fi
fi

mkdir -p "$OUT_DIR" "$MODULES_DIR"
rm -f "$MODULES_DIR"/*.ko(N)
rm -f "$OUT_DIR"/*.ko(N)

scp "$REMOTE_LINUX_HOST:$REMOTE_IMAGE_PATH" "$OUT_DIR/Image"

sync_optional_module_from_config "CONFIG_UB_HISI_UBUS" "$REMOTE_HISI_MODULE_PATH" "$MODULES_DIR/hisi_ubus.ko"
sync_optional_module_from_config "CONFIG_UB_UBUS_BUS" "$REMOTE_UBUS_MODULE_PATH" "$MODULES_DIR/ubus.ko"
sync_optional_module_from_config "CONFIG_UB_UBUS_SIM_DECODER" "$REMOTE_UB_SIM_DECODER_MODULE_PATH" "$MODULES_DIR/ub-sim-decoder.ko"
sync_optional_module_from_config "CONFIG_OBMM" "$REMOTE_OBMM_MODULE_PATH" "$MODULES_DIR/obmm.ko"
sync_optional_module_from_config "CONFIG_UB_UBASE" "$REMOTE_UBASE_MODULE_PATH" "$MODULES_DIR/ubase.ko"
sync_optional_module_from_config "CONFIG_UB_URMA" "$REMOTE_UBCORE_MODULE_PATH" "$MODULES_DIR/ubcore.ko"
sync_optional_module_from_config "CONFIG_UB_UDMA" "$REMOTE_UDMA_MODULE_PATH" "$MODULES_DIR/udma.ko"
sync_optional_module_from_config "CONFIG_UB_URMA" "$REMOTE_IPOURMA_MODULE_PATH" "$MODULES_DIR/ipourma.ko"
sync_optional_module_from_config "CONFIG_UB_URMA" "$REMOTE_UBURMA_MODULE_PATH" "$MODULES_DIR/uburma.ko"
sync_optional_module_from_config "CONFIG_UB_UMMU_CORE_DRIVER" "$REMOTE_UMMU_CORE_MODULE_PATH" "$MODULES_DIR/ummu-core.ko"
sync_optional_module_from_config "CONFIG_UB_UMMU" "$REMOTE_UMMU_MODULE_PATH" "$MODULES_DIR/ummu.ko"
if [[ -n "$REMOTE_LINQU_MODULE_PATH" ]]; then
  copy_optional "$REMOTE_LINQU_MODULE_PATH" "$MODULES_DIR/linqu_ub_drv.ko"
elif [[ -n "$REMOTE_LINQU_DRIVER_DIR" ]]; then
  copy_optional "$REMOTE_LINQU_DRIVER_DIR/linqu_ub_drv.ko" "$MODULES_DIR/linqu_ub_drv.ko"
fi

echo "[sync] done:"
echo "[sync]   $OUT_DIR/Image"
for mod in hisi_ubus.ko ubus.ko ub-sim-decoder.ko obmm.ko ubase.ko ubcore.ko udma.ko ipourma.ko uburma.ko ummu-core.ko ummu.ko linqu_ub_drv.ko; do
  if [[ -f "$MODULES_DIR/$mod" ]]; then
    echo "[sync]   $MODULES_DIR/$mod"
  fi
done
