#!/bin/zsh

run_gnu_make() {
  if command -v gmake >/dev/null 2>&1; then
    command gmake "$@"
    return
  fi
  command make "$@"
}

ensure_sim_kernel_append_defaults() {
  local append_extra="${1:-}"

  if [[ "$append_extra" != *"obmm.skip_cache_maintain="* ]]; then
    append_extra="${append_extra} obmm.skip_cache_maintain=1"
  fi
  if [[ "$append_extra" != *"rcupdate.rcu_cpu_stall_timeout="* ]]; then
    append_extra="${append_extra} rcupdate.rcu_cpu_stall_timeout=300"
  fi

  echo "${append_extra## }"
}

ensure_simpler_host_manifest() {
  local script_dir="$1"
  local profile="$2"
  local manifest="$3"
  local producer=""

  if [[ -f "$manifest" ]]; then
    echo "$manifest"
    return 0
  fi

  case "$profile" in
    host_vector)
      producer="$script_dir/prepare_simpler_host_vector_artifacts.sh"
      ;;
    host_matmul|qwen3_dense_reference|qwen3_dense)
      producer="$script_dir/prepare_simpler_host_matmul_artifacts.sh"
      ;;
    *)
      echo "[ub_common] unsupported simpler host artifact profile: $profile" >&2
      return 1
      ;;
  esac

  if [[ ! -x "$producer" ]]; then
    echo "[ub_common] missing executable artifact producer: $producer" >&2
    return 1
  fi
  "$producer" "$(dirname "$manifest")"
}

is_qwen3_dense_w4_profile() {
  local profile="$1"
  [[ "$profile" == "qwen3_dense_reference" || "$profile" == "qwen3_dense" ]]
}

qwen3_dense_apply_config_env() {
  local profile="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}"
  local weights_path="${SIM_QWEN3_DENSE_WEIGHTS_PATH:-}"

  if ! is_qwen3_dense_w4_profile "$profile"; then
    return 0
  fi
  if [[ -z "$weights_path" ]]; then
    return 0
  fi

  eval "$(python3 - "$weights_path" "$profile" <<'PY'
import json
import os
import shlex
import sys
from pathlib import Path

weights_path = Path(sys.argv[1])
profile = sys.argv[2]
config_path = weights_path / "config.json"
with config_path.open("r", encoding="utf-8") as f:
    config = json.load(f)

def require_int(key):
    value = config.get(key)
    if not isinstance(value, int):
        raise SystemExit(f"qwen3 config missing integer {key}: {config_path}")
    return value

def model_key(model_id):
    tail = model_id.rsplit("/", 1)[-1]
    out = []
    previous_dash = False
    for ch in tail.lower():
        if ch.isalnum() and ch.isascii():
            out.append(ch)
            previous_dash = False
        elif not previous_dash:
            out.append("-")
            previous_dash = True
    key = "".join(out).rstrip("-")
    return key or "qwen3-dense"

def env_int(name, fallback):
    value = os.environ.get(name)
    if value:
        try:
            return int(value)
        except ValueError:
            pass
    return fallback

model_id = (
    os.environ.get("SIM_QWEN3_DENSE_MODEL_ID")
    or config.get("_name_or_path")
    or config.get("model_id")
    or weights_path.name
)
vocab_size = require_int("vocab_size")
hidden_size = require_int("hidden_size")
intermediate_size = require_int("intermediate_size")
num_hidden_layers = require_int("num_hidden_layers")
num_attention_heads = require_int("num_attention_heads")
num_key_value_heads = require_int("num_key_value_heads")
head_dim = require_int("head_dim")
prefill_tokens = env_int("SIM_QWEN3_DENSE_PREFILL_TOKENS", 128)
decode_tokens = env_int("SIM_QWEN3_DENSE_DECODE_TOKENS", 1)
tp_nodes = env_int("SIM_QWEN3_DENSE_TP_NODES", 8)
hidden_range_bytes = env_int(
    "SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES",
    prefill_tokens * hidden_size * 2,
)
decode_hidden_bytes = env_int(
    "SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES",
    decode_tokens * hidden_size * 2,
)
kv_state_bytes = env_int(
    "SIM_QWEN3_DENSE_KV_STATE_BYTES",
    num_hidden_layers * decode_tokens * num_key_value_heads * head_dim * 2 * 4,
)

is_reference = (
    vocab_size == 151936
    and hidden_size == 1024
    and intermediate_size == 3072
    and num_hidden_layers == 28
    and num_attention_heads == 16
    and num_key_value_heads == 8
    and head_dim == 128
)
resolved_profile = "qwen3_dense_reference" if profile == "qwen3_dense_reference" and is_reference else profile
if profile == "qwen3_dense_reference" and not is_reference:
    resolved_profile = "qwen3_dense"

values = {
    "SIM_UAPI_W4_CHIPBACKEND_PROFILE": resolved_profile,
    "SIM_QWEN3_DENSE_MODEL_ID": model_id,
    "SIM_QWEN3_DENSE_MODEL_KEY": os.environ.get("SIM_QWEN3_DENSE_MODEL_KEY") or model_key(model_id),
    "SIM_QWEN3_DENSE_WEIGHTS_PATH": str(weights_path),
    "SIM_QWEN3_DENSE_VOCAB_SIZE": str(vocab_size),
    "SIM_QWEN3_DENSE_HIDDEN_SIZE": str(hidden_size),
    "SIM_QWEN3_DENSE_INTERMEDIATE_SIZE": str(intermediate_size),
    "SIM_QWEN3_DENSE_NUM_HIDDEN_LAYERS": str(num_hidden_layers),
    "SIM_QWEN3_DENSE_NUM_ATTENTION_HEADS": str(num_attention_heads),
    "SIM_QWEN3_DENSE_NUM_KEY_VALUE_HEADS": str(num_key_value_heads),
    "SIM_QWEN3_DENSE_HEAD_DIM": str(head_dim),
    "SIM_QWEN3_DENSE_PREFILL_TOKENS": str(prefill_tokens),
    "SIM_QWEN3_DENSE_DECODE_TOKENS": str(decode_tokens),
    "SIM_QWEN3_DENSE_TP_NODES": str(tp_nodes),
    "SIM_QWEN3_DENSE_HIDDEN_RANGE_BYTES": str(hidden_range_bytes),
    "SIM_QWEN3_DENSE_DECODE_HIDDEN_BYTES": str(decode_hidden_bytes),
    "SIM_QWEN3_DENSE_KV_STATE_BYTES": str(kv_state_bytes),
}
for key, value in values.items():
    print(f"export {key}={shlex.quote(value)}")
PY
)"
}

qemu_ub_bin_path() {
  local workspace_root="$1"
  local build_dir="$workspace_root/vendor/qemu_8.2.0_ub/build"
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
  echo "$workspace_root/vendor/qemu_8.2.0_ub/build"
}

qemu_ub_source_path() {
  local workspace_root="$1"
  echo "$workspace_root/vendor/qemu_8.2.0_ub"
}

qemu_ub_supports_required_opts() {
  local bin="$1"
  local help=""
  help="$("$bin" -M virt,help 2>/dev/null || true)"
  [[ -n "$help" ]] || return 1
  print -r -- "$help" | grep -Eq "ub-cluster-mode|ummu"
}

print_qemu_preflight_help() {
  local workspace_root="$1"
  local src_dir="$2"
  local build_dir="$3"
  local bin="$4"
  local helper_script="$workspace_root/guest-linux/aarch64/scripts/build_qemu_binary.sh"

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
  local cc_hint="${5:-aarch64-*-gnu-gcc}"
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
[ub_common]   REMOTE_LINUX_HOST=user@build-host REMOTE_KERNEL_SRC=/path/to/kernel_ub REMOTE_KERNEL_BUILD=/path/to/kernel_build BUILD_ON_REMOTE=1 BUILD_LINQU_DRIVER_ON_REMOTE=1 ./scripts/sync_ub_kernel_artifacts_from_remote_linux.sh
[ub_common]   AARCH64_LINUX_CC=$cc_hint BUSYBOX=\$PWD/busybox-aarch64 ./scripts/build_initramfs.sh
[ub_common] or pass explicit overrides:
[ub_common]   KERNEL_IMAGE=/path/to/Image INITRAMFS_IMAGE=/path/to/initramfs.cpio.gz ./scripts/launch_ub_dual_node_tmux.sh
EOF
}

detect_aarch64_linux_cc() {
  emulate -L zsh
  setopt null_glob
  local cc
  local native_cc
  local native_target
  if [[ -n "${AARCH64_LINUX_CC:-}" ]]; then
    echo "$AARCH64_LINUX_CC"
    return 0
  fi
  native_cc="$(command -v gcc 2>/dev/null || true)"
  if [[ -n "$native_cc" ]]; then
    native_target="$($native_cc -dumpmachine 2>/dev/null || true)"
    if [[ "$native_target" == aarch64* ]]; then
      echo "$native_cc"
      return 0
    fi
  fi
  for cc in aarch64-*-gnu-gcc /usr/bin/aarch64-*-gnu-gcc /opt/homebrew/bin/aarch64-*-gnu-gcc /opt/local/bin/aarch64-*-gnu-gcc; do
    if command -v "$cc" >/dev/null 2>&1; then
      command -v "$cc"
      return 0
    fi
    if [[ -x "$cc" ]]; then
      echo "$cc"
      return 0
    fi
  done
  echo ""
}

aarch64_linux_cross_prefix() {
  local cc_path="$1"
  local cc_name=""
  local compiler_target=""
  local host_arch=""

  compiler_target="$("$cc_path" -dumpmachine 2>/dev/null || true)"
  host_arch="$(uname -m 2>/dev/null || true)"
  if [[ "$(uname -s 2>/dev/null || true)" == "Linux" && \
        "$host_arch" == (aarch64|arm64) && \
        "$compiler_target" == aarch64* ]]; then
    echo ""
    return 0
  fi

  cc_name="$(basename "$cc_path")"
  if [[ "$cc_name" != *gcc ]]; then
    return 1
  fi
  echo "${cc_path%gcc}"
}

ensure_ub_guest_artifacts() {
  local guest_root="$1"
  local kernel_image="$2"
  local initramfs_image="$3"
  local out_dir="$guest_root/out"
  local default_kernel="$out_dir/Image"
  local default_initramfs="$out_dir/initramfs.cpio.gz"
  local modules_dir="${UB_GUEST_MODULES_DIR:-$out_dir/modules}"
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
    echo "[ub_common] preparing guest artifacts via build_guest_artifacts.sh (source=$artifact_source)" >&2
    if ! (
      cd "$guest_root"
      ARTIFACT_SOURCE="$artifact_source" \
      BUILD_ON_REMOTE="${UB_SYNC_BUILD_ON_REMOTE:-0}" \
      BUILD_LINQU_DRIVER_ON_REMOTE="${UB_SYNC_BUILD_LINQU_ON_REMOTE:-0}" \
      SYNC_KERNEL_SRC_TO_REMOTE="${UB_SYNC_KERNEL_SRC_TO_REMOTE:-0}" \
      ALLOW_REMOTE_LINUX_ARTIFACTS="${UB_ALLOW_REMOTE_LINUX_ARTIFACTS:-0}" \
      REMOTE_LINUX_HOST="${UB_REMOTE_LINUX_HOST:-}" \
      REMOTE_KERNEL_SRC="${UB_REMOTE_KERNEL_SRC:-}" \
      REMOTE_KERNEL_BUILD="${UB_REMOTE_KERNEL_BUILD:-}" \
      REMOTE_TMPDIR="${UB_REMOTE_TMPDIR:-}" \
      REMOTE_LINQU_DRIVER_DIR="${UB_REMOTE_LINQU_DRIVER_DIR:-}" \
      REMOTE_LINQU_MODULE_PATH="${UB_REMOTE_LINQU_MODULE_PATH:-}" \
      REMOTE_REUSE_KERNEL_CONFIG="${UB_REMOTE_REUSE_KERNEL_CONFIG:-0}" \
      AARCH64_LINUX_CC="$(detect_aarch64_linux_cc)" \
      BUSYBOX="${BUSYBOX:-}" \
      LOCAL_KERNEL_IMAGE="${UB_LOCAL_KERNEL_IMAGE:-}" \
      LOCAL_MODULES_DIR="${UB_LOCAL_MODULES_DIR:-}" \
      zsh ./scripts/build_guest_artifacts.sh
    ); then
      echo "[ub_common] build_guest_artifacts.sh failed" >&2
      print_guest_preflight_help "$guest_root" "$default_kernel" "$default_initramfs" "$modules_dir" "$(detect_aarch64_linux_cc)"
      return 1
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
      AARCH64_LINUX_CC="$cc" BUSYBOX="$busybox_bin" \
        zsh ./scripts/build_initramfs.sh >/dev/null
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

  if [[ "${UB_USE_PREBUILT_QEMU:-0}" == "1" ]]; then
    if [[ ! -x "$bin" ]]; then
      echo "prebuilt QEMU binary not found or not executable: $bin" >&2
      print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
      return 1
    fi
    if ! qemu_ub_supports_required_opts "$bin"; then
      echo "prebuilt QEMU binary missing UB machine options (ummu/ub-cluster-mode): $bin" >&2
      print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
      return 1
    fi
    echo "[ub_common] using verified prebuilt QEMU binary: $bin" >&2
    echo "$bin"
    return 0
  fi

  if [[ ! -d "$src_dir" ]]; then
    echo "QEMU source dir not found: $src_dir" >&2
    print_qemu_preflight_help "$workspace_root" "$src_dir" "$build_dir" "$bin"
    return 1
  fi
  if ! (
    cd "$workspace_root/guest-linux/aarch64"
    QEMU_BUILD_JOBS="$jobs" zsh ./scripts/build_qemu_binary.sh >/dev/null
  ); then
    echo "[ub_common] build_qemu_binary.sh failed" >&2
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

# ---------------------------------------------------------------------------
# openEuler guest helpers (shared by W5 openEuler engine)
# ---------------------------------------------------------------------------

oe_privileged() {
  if [[ "$(id -u)" == "0" ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

oe_ensure_lvm2_staging() {
  # $1 = openEuler qcow2 disk image; extracts LVM2 userland once into
  # $OE_LVM2_STAGING_DIR so the initramfs can activate the root LVM volume.
  local disk_image="$1"
  local staging_dir="${OE_LVM2_STAGING_DIR:-/tmp/oe_lvm2_tools}"
  local raw_img loop_dev mnt_dir root_dev bin lib cfg bin_path lib_name dep dep_src dep_dst
  local -a root_candidates

  export OE_LVM2_STAGING_DIR="$staging_dir"
  if [[ -r "$staging_dir/direct-root" ||
        ( -r "$staging_dir/lvm" && -r "$staging_dir/vgscan" ) ]]; then
    return 0
  fi
  [[ -f "$disk_image" ]] || { echo "openEuler disk image not found: $disk_image" >&2; return 1; }

  echo "[ub_common] extracting LVM2 tools from openEuler disk..." >&2
  rm -rf "$staging_dir"
  mkdir -p "$staging_dir"

  raw_img="/tmp/oe_lvm2_extract_$$.raw"
  qemu-img convert -f qcow2 -O raw -S 512 "$disk_image" "$raw_img" || {
    rm -f "$raw_img"; return 1; }

  loop_dev="$(oe_privileged losetup -f --show -P "$raw_img")"
  mnt_dir="/mnt/oe_lvm2_extract_$$"
  oe_privileged mkdir -p "$mnt_dir"

  if oe_privileged mount "$loop_dev" "$mnt_dir" 2>/dev/null; then
    oe_privileged touch "$staging_dir/direct-root"
    oe_privileged chown -R "$(id -u):$(id -g)" "$staging_dir"
    oe_privileged umount "$mnt_dir"
    oe_privileged rmdir "$mnt_dir"
    oe_privileged losetup -d "$loop_dev"
    rm -f "$raw_img"
    echo "[ub_common] openEuler disk uses a direct root filesystem" >&2
    return 0
  fi

  oe_privileged partprobe "$loop_dev"
  oe_privileged pvscan >/dev/null
  oe_privileged vgscan >/dev/null
  oe_privileged vgchange -ay >/dev/null
  root_dev=/dev/mapper/openeuler_bogon-root
  if [[ ! -b "$root_dev" ]]; then
    root_candidates=(/dev/mapper/*root*(N))
    root_dev="${root_candidates[1]:-}"
  fi
  if [[ -z "$root_dev" || ! -b "$root_dev" ]]; then
    echo "openEuler root LVM volume was not activated" >&2
    oe_privileged vgchange -an >/dev/null 2>&1 || true
    oe_privileged rmdir "$mnt_dir" 2>/dev/null || true
    oe_privileged losetup -d "$loop_dev" 2>/dev/null || true
    rm -f "$raw_img"
    return 1
  fi
  oe_privileged mount "$root_dev" "$mnt_dir" || {
    oe_privileged vgchange -an >/dev/null 2>&1 || true
    oe_privileged losetup -d "$loop_dev" 2>/dev/null || true
    rm -f "$raw_img"; return 1; }

  for bin in lvm vgscan vgchange pvscan dmsetup; do
    if [[ -f "$mnt_dir/usr/sbin/$bin" ]]; then
      oe_privileged cp -L "$mnt_dir/usr/sbin/$bin" "$staging_dir/"
    fi
  done
  oe_privileged mkdir -p "$staging_dir/etc/lvm"
  for cfg in lvm.conf lvmlocal.conf; do
    if [[ -f "$mnt_dir/etc/lvm/$cfg" ]]; then
      oe_privileged cp -L "$mnt_dir/etc/lvm/$cfg" "$staging_dir/etc/lvm/"
    fi
  done
  for bin_path in /usr/sbin/lvm /usr/sbin/dmsetup; do
    [[ -f "$mnt_dir$bin_path" ]] || continue
    oe_privileged chroot "$mnt_dir" /usr/bin/ldd "$bin_path" 2>/dev/null \
      | grep '=> /' | awk '{print $3}' | while read -r lib; do
        if [[ -f "$mnt_dir$lib" ]]; then
          oe_privileged cp -L "$mnt_dir$lib" "$staging_dir/"
        fi
      done
  done
  for lib in "$staging_dir"/*.so*; do
    [[ -f "$lib" ]] || continue
    lib_name="$(basename "$lib")"
    oe_privileged chroot "$mnt_dir" /usr/bin/ldd "/tmp/../$lib_name" >/dev/null 2>&1 || true
    oe_privileged chroot "$mnt_dir" /usr/bin/ldd "$lib_name" 2>/dev/null \
      | grep '=> /' | awk '{print $3}' | while read -r dep; do
        dep_src="$mnt_dir$dep"
        dep_dst="$staging_dir/$(basename "$dep")"
        if [[ -f "$dep_src" && ! -f "$dep_dst" ]]; then
          oe_privileged cp -L "$dep_src" "$dep_dst"
        fi
      done
  done
  if [[ -f "$mnt_dir/lib/ld-linux-aarch64.so.1" ]]; then
    oe_privileged cp -L "$mnt_dir/lib/ld-linux-aarch64.so.1" "$staging_dir/"
  elif [[ -f "$mnt_dir/lib64/ld-linux-aarch64.so.1" ]]; then
    oe_privileged cp -L "$mnt_dir/lib64/ld-linux-aarch64.so.1" "$staging_dir/"
  fi

  oe_privileged chown -R "$(id -u):$(id -g)" "$staging_dir"
  chmod +x "$staging_dir"/* 2>/dev/null || true

  oe_privileged umount "$mnt_dir" 2>/dev/null || true
  oe_privileged rmdir "$mnt_dir" 2>/dev/null || true
  oe_privileged vgchange -an >/dev/null 2>&1 || true
  oe_privileged losetup -d "$loop_dev" 2>/dev/null || true
  rm -f "$raw_img"
  echo "[ub_common] LVM2 staging ready at $staging_dir" >&2
}

oe_build_boot_skeleton() {
  # $1 = guest-linux/aarch64 root dir (hosts initramfs/init_switch_root, out/)
  # $2 = busybox static binary
  # $3 = target initramfs tree; populated with busybox, /init (init_switch_root),
  #      LVM2 tools and the UB kernel modules that ship as =m.
  local root_dir="$1"
  local busybox="$2"
  local tree="$3"
  local staging_dir="${OE_LVM2_STAGING_DIR:-/tmp/oe_lvm2_tools}"
  local mod_src ko cmd

  mkdir -p "$tree/bin" "$tree/sbin" "$tree/lib" "$tree/lib/modules" "$tree/etc"
  cp -L "$busybox" "$tree/bin/busybox"
  chmod +x "$tree/bin/busybox"
  for cmd in sh echo cat mount umount mkdir sleep ls cp mv rm basename readlink insmod modprobe switch_root; do
    ln -sf busybox "$tree/bin/$cmd" 2>/dev/null || true
  done

  cp "$root_dir/initramfs/init_switch_root" "$tree/init"
  chmod +x "$tree/init"

  if [[ -d "$staging_dir" ]]; then
    for cmd in lvm vgscan vgchange pvscan dmsetup; do
      if [[ -f "$staging_dir/$cmd" ]]; then
        cp -L "$staging_dir/$cmd" "$tree/sbin/"
      fi
    done
    for cmd in "$staging_dir"/*.so* "$staging_dir"/ld-linux*; do
      [[ -f "$cmd" ]] || continue
      cp -L "$cmd" "$tree/lib/"
    done
    if [[ -d "$staging_dir/etc/lvm" ]]; then
      cp -r "$staging_dir/etc/lvm" "$tree/etc/"
    fi
  fi

  mod_src="$root_dir/out/kernel_build"
  for ko in \
      "drivers/ub/ubase/ubase.ko" \
      "drivers/ub/urma/ubcore/ubcore.ko" \
      "drivers/ub/urma/hw/udma/udma.ko" \
      "drivers/ub/urma/ulp/ipourma/ipourma.ko" \
      "drivers/ub/urma/uburma/uburma.ko" \
      "drivers/iommu/hisilicon/ummu-core/ummu-core.ko" \
      "drivers/iommu/hisilicon/ummu.ko"; do
    if [[ -f "$mod_src/$ko" ]]; then
      cp "$mod_src/$ko" "$tree/lib/modules/"
    fi
  done
}
