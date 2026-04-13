#!/bin/zsh

ensure_sim_kernel_append_defaults() {
  local append_extra="${1:-}"

  if [[ "$append_extra" != *"obmm.skip_cache_maintain="* ]]; then
    append_extra="${append_extra} obmm.skip_cache_maintain=1"
  fi

  echo "${append_extra## }"
}

qemu_ub_bin_path() {
  local repo_root="$1"
  echo "$repo_root/vendor/qemu_8.2.0_ub/build/qemu-system-aarch64"
}

qemu_ub_build_path() {
  local repo_root="$1"
  echo "$repo_root/vendor/qemu_8.2.0_ub/build"
}

qemu_ub_source_path() {
  local repo_root="$1"
  echo "$repo_root/vendor/qemu_8.2.0_ub"
}

qemu_ub_supports_required_opts() {
  local bin="$1"
  "$bin" -M virt,help 2>/dev/null | rg -q "ub-cluster-mode|ummu"
}

detect_aarch64_linux_cc() {
  if [[ -n "${AARCH64_LINUX_CC:-}" ]]; then
    echo "$AARCH64_LINUX_CC"
    return 0
  fi
  if command -v aarch64-unknown-linux-gnu-gcc >/dev/null 2>&1; then
    command -v aarch64-unknown-linux-gnu-gcc
    return 0
  fi
  if [[ -x /opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc ]]; then
    echo "/opt/homebrew/bin/aarch64-unknown-linux-gnu-gcc"
    return 0
  fi
  echo ""
}

ensure_ub_guest_artifacts() {
  local guest_root="$1"
  local kernel_image="$2"
  local initramfs_image="$3"
  local out_dir="$guest_root/out"
  local default_kernel="$out_dir/Image"
  local default_initramfs="$out_dir/initramfs.cpio.gz"
  local modules_dir="${UB_GUEST_MODULES_DIR:-$out_dir/modules}"
  local need_sync=0
  local cc
  local required_modules=("hisi_ubus.ko" "udma.ko")
  local mod=""

  if [[ "$kernel_image" != "$default_kernel" || "$initramfs_image" != "$default_initramfs" ]]; then
    if [[ ! -f "$kernel_image" ]]; then
      echo "KERNEL_IMAGE not found: $kernel_image" >&2
      return 1
    fi
    if [[ ! -f "$initramfs_image" ]]; then
      echo "INITRAMFS_IMAGE not found: $initramfs_image" >&2
      return 1
    fi
    return 0
  fi

  if [[ "${UB_SYNC_ARTIFACTS:-1}" == "1" ]]; then
    if [[ "${UB_FORCE_SYNC_ARTIFACTS:-0}" == "1" || ! -f "$default_kernel" ]]; then
      need_sync=1
    fi
    for mod in "${required_modules[@]}"; do
      if [[ ! -f "$modules_dir/$mod" ]]; then
        need_sync=1
      fi
    done

    if (( need_sync )); then
      echo "[ub_common] syncing guest kernel artifacts from VM" >&2
      (
        cd "$guest_root"
        BUILD_IN_VM="${UB_SYNC_BUILD_IN_VM:-1}" \
        BUILD_LINQU_DRIVER_IN_VM="${UB_SYNC_BUILD_LINQU_IN_VM:-1}" \
        ./scripts/sync_ub_kernel_artifacts_from_vm.sh
      )
    fi
  fi

  if [[ "${UB_REBUILD_INITRAMFS:-1}" == "1" ]]; then
    cc="$(detect_aarch64_linux_cc)"
    if [[ -z "$cc" ]]; then
      echo "AARCH64_LINUX_CC is required to rebuild initramfs" >&2
      return 1
    fi
    local busybox_bin="${BUSYBOX:-}"
    if [[ -z "$busybox_bin" ]] && [[ -x "$guest_root/busybox-aarch64" ]]; then
      busybox_bin="$guest_root/busybox-aarch64"
    fi
    echo "[ub_common] rebuilding initramfs" >&2
    (
      cd "$guest_root"
      AARCH64_LINUX_CC="$cc" BUSYBOX="$busybox_bin" ./scripts/build_initramfs.sh >/dev/null
    )
  fi

  if [[ ! -f "$default_kernel" ]]; then
    echo "KERNEL_IMAGE not found: $default_kernel" >&2
    return 1
  fi
  if [[ ! -f "$default_initramfs" ]]; then
    echo "INITRAMFS_IMAGE not found: $default_initramfs" >&2
    return 1
  fi
}

ensure_qemu_ub_binary() {
  local workspace_root="$1"
  local src_dir
  local build_dir
  local bin
  local jobs

  src_dir="$(qemu_ub_source_path "$workspace_root")"
  build_dir="$(qemu_ub_build_path "$workspace_root")"
  bin="$(qemu_ub_bin_path "$workspace_root")"
  jobs="${QEMU_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8)}"

  if [[ ! -d "$src_dir" ]]; then
    echo "QEMU source dir not found: $src_dir" >&2
    return 1
  fi
  if [[ ! -f "$build_dir/build.ninja" ]]; then
    echo "QEMU build.ninja missing: $build_dir/build.ninja" >&2
    echo "Run configure first in $src_dir (out-of-tree build dir: $build_dir)." >&2
    return 1
  fi

  (
    cd "$build_dir"
    ninja -j"$jobs" qemu-system-aarch64 >/dev/null
  )

  if [[ ! -x "$bin" ]]; then
    echo "QEMU binary not found after build: $bin" >&2
    return 1
  fi
  if ! qemu_ub_supports_required_opts "$bin"; then
    echo "QEMU binary missing UB machine options (ummu/ub-cluster-mode): $bin" >&2
    return 1
  fi

  echo "$bin"
}
