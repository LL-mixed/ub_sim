#!/bin/zsh

ensure_sim_kernel_append_defaults() {
  local append_extra="${1:-}"

  if [[ "$append_extra" != *"obmm.skip_cache_maintain="* ]]; then
    append_extra="${append_extra} obmm.skip_cache_maintain=1"
  fi

  echo "${append_extra## }"
}

qemu_ub_bin_path() {
  local workspace_root="$1"
  local build_dir="$workspace_root/simulator/vendor/qemu_8.2.0_ub/build"
  local signed_bin="$build_dir/qemu-system-aarch64"
  local unsigned_bin="$build_dir/qemu-system-aarch64-unsigned"

  if [[ -x "$signed_bin" ]]; then
    echo "$signed_bin"
    return 0
  fi
  if [[ -x "$unsigned_bin" ]]; then
    echo "$unsigned_bin"
    return 0
  fi
  echo "$signed_bin"
}

qemu_ub_build_path() {
  local workspace_root="$1"
  echo "$workspace_root/simulator/vendor/qemu_8.2.0_ub/build"
}

qemu_ub_source_path() {
  local workspace_root="$1"
  echo "$workspace_root/simulator/vendor/qemu_8.2.0_ub"
}

qemu_ub_supports_required_opts() {
  local bin="$1"
  "$bin" -M virt,help 2>/dev/null | rg -q "ub-cluster-mode|ummu"
}

print_qemu_preflight_help() {
  local workspace_root="$1"
  local src_dir="$2"
  local build_dir="$3"
  local bin="$4"
  local helper_script="$workspace_root/simulator/guest-linux/aarch64/scripts/build_qemu_binary.sh"

  cat >&2 <<EOF
[ub_common] qemu preflight failed
[ub_common] expected source: $src_dir
[ub_common] expected build dir: $build_dir
[ub_common] expected binary: $bin
[ub_common] suggested script:
[ub_common]   $helper_script
[ub_common] manual fallback:
[ub_common]   cd $src_dir
[ub_common]   mkdir -p build
[ub_common]   cd build
[ub_common]   ../configure --target-list=aarch64-softmmu
[ub_common]   ninja -j8 qemu-system-aarch64
EOF
}

print_guest_preflight_help() {
  local guest_root="$1"
  local kernel_image="$2"
  local initramfs_image="$3"
  local modules_dir="$4"
  local cc_hint="${5:-aarch64-unknown-linux-gnu-gcc}"
  local helper_script="$guest_root/scripts/build_guest_artifacts.sh"

  cat >&2 <<EOF
[ub_common] guest artifact preflight failed
[ub_common] expected kernel image: $kernel_image
[ub_common] expected initramfs: $initramfs_image
[ub_common] expected modules dir: $modules_dir
[ub_common] suggested script:
[ub_common]   cd $guest_root
[ub_common]   AARCH64_LINUX_CC=$cc_hint BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_guest_artifacts.sh
[ub_common] busybox helper:
[ub_common]   cd $guest_root
[ub_common]   AARCH64_LINUX_CC=$cc_hint ./scripts/prepare_busybox.sh
[ub_common] manual fallback:
[ub_common]   cd $guest_root
[ub_common]   BUILD_IN_VM=1 BUILD_LINQU_DRIVER_IN_VM=1 ./scripts/sync_ub_kernel_artifacts_from_vm.sh
[ub_common]   AARCH64_LINUX_CC=$cc_hint BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_initramfs.sh
[ub_common] or pass explicit overrides:
[ub_common]   KERNEL_IMAGE=/path/to/Image INITRAMFS_IMAGE=/path/to/initramfs.cpio.gz ./scripts/launch_ub_dual_node_tmux.sh
EOF
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
  local artifact_source="${UB_GUEST_ARTIFACT_SOURCE:-auto}"

  if [[ "$kernel_image" != "$default_kernel" || "$initramfs_image" != "$default_initramfs" ]]; then
    if [[ ! -f "$kernel_image" ]]; then
      echo "KERNEL_IMAGE not found: $kernel_image" >&2
      print_guest_preflight_help "$guest_root" "$kernel_image" "$initramfs_image" "$modules_dir" "$(detect_aarch64_linux_cc)"
      return 1
    fi
    if [[ ! -f "$initramfs_image" ]]; then
      echo "INITRAMFS_IMAGE not found: $initramfs_image" >&2
      print_guest_preflight_help "$guest_root" "$kernel_image" "$initramfs_image" "$modules_dir" "$(detect_aarch64_linux_cc)"
      return 1
    fi
    return 0
  fi

  if [[ "${UB_SYNC_ARTIFACTS:-1}" == "1" ]]; then
    if [[ "${UB_FORCE_SYNC_ARTIFACTS:-0}" == "1" || ! -f "$default_kernel" ]]; then
      need_sync=1
    fi
    if [[ ! -d "$modules_dir" ]]; then
      need_sync=1
    fi

    if (( need_sync )); then
      echo "[ub_common] preparing guest artifacts via build_guest_artifacts.sh (source=$artifact_source)" >&2
      if ! (
        cd "$guest_root"
        ARTIFACT_SOURCE="$artifact_source" \
        BUILD_IN_VM="${UB_SYNC_BUILD_IN_VM:-1}" \
        BUILD_LINQU_DRIVER_IN_VM="${UB_SYNC_BUILD_LINQU_IN_VM:-1}" \
        AARCH64_LINUX_CC="$(detect_aarch64_linux_cc)" \
        BUSYBOX="${BUSYBOX:-}" \
        LOCAL_KERNEL_IMAGE="${UB_LOCAL_KERNEL_IMAGE:-}" \
        LOCAL_MODULES_DIR="${UB_LOCAL_MODULES_DIR:-}" \
        ./scripts/build_guest_artifacts.sh
      ); then
        echo "[ub_common] build_guest_artifacts.sh failed" >&2
        print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir" "$(detect_aarch64_linux_cc)"
        return 1
      fi
    fi
  fi

  if [[ "${UB_REBUILD_INITRAMFS:-1}" == "1" && ! -f "$default_initramfs" ]]; then
    cc="$(detect_aarch64_linux_cc)"
    if [[ -z "$cc" ]]; then
      echo "AARCH64_LINUX_CC is required to rebuild initramfs" >&2
      print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir"
      return 1
    fi
    local busybox_bin="${BUSYBOX:-}"
    if [[ -z "$busybox_bin" ]] && [[ -x "$guest_root/busybox-aarch64" ]]; then
      busybox_bin="$guest_root/busybox-aarch64"
    fi
    echo "[ub_common] rebuilding initramfs" >&2
    if ! (
      cd "$guest_root"
      AARCH64_LINUX_CC="$cc" BUSYBOX="$busybox_bin" ./scripts/build_initramfs.sh >/dev/null
    ); then
      echo "[ub_common] build_initramfs.sh failed" >&2
      print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir" "$cc"
      return 1
    fi
  fi

  if [[ ! -f "$default_kernel" ]]; then
    echo "KERNEL_IMAGE not found: $default_kernel" >&2
    print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir" "$(detect_aarch64_linux_cc)"
    return 1
  fi
  if [[ ! -f "$default_initramfs" ]]; then
    echo "INITRAMFS_IMAGE not found: $default_initramfs" >&2
    print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir" "$(detect_aarch64_linux_cc)"
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
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi
  if [[ ! -f "$build_dir/build.ninja" ]]; then
    echo "QEMU build.ninja missing: $build_dir/build.ninja" >&2
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi

  if ! (
    cd "$build_dir"
    ninja -j"$jobs" qemu-system-aarch64 >/dev/null
  ); then
    echo "[ub_common] ninja qemu-system-aarch64 failed" >&2
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi

  if [[ ! -x "$bin" ]]; then
    echo "QEMU binary not found after build: $bin" >&2
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi
  if ! qemu_ub_supports_required_opts "$bin"; then
    echo "QEMU binary missing UB machine options (ummu/ub-cluster-mode): $bin" >&2
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi

  echo "$bin"
}
