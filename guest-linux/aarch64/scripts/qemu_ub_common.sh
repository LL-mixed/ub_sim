#!/bin/zsh

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
  if [[ -n "${AARCH64_LINUX_CC:-}" ]]; then
    echo "$AARCH64_LINUX_CC"
    return 0
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
      ALLOW_REMOTE_LINUX_ARTIFACTS="${UB_ALLOW_REMOTE_LINUX_ARTIFACTS:-0}" \
      REMOTE_LINUX_HOST="${UB_REMOTE_LINUX_HOST:-}" \
      REMOTE_KERNEL_SRC="${UB_REMOTE_KERNEL_SRC:-}" \
      REMOTE_KERNEL_BUILD="${UB_REMOTE_KERNEL_BUILD:-}" \
      REMOTE_LINQU_DRIVER_DIR="${UB_REMOTE_LINQU_DRIVER_DIR:-}" \
      REMOTE_LINQU_MODULE_PATH="${UB_REMOTE_LINQU_MODULE_PATH:-}" \
      REMOTE_REUSE_KERNEL_CONFIG="${UB_REMOTE_REUSE_KERNEL_CONFIG:-0}" \
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
  if ! (
    cd "$workspace_root/guest-linux/aarch64"
    QEMU_BUILD_JOBS="$jobs" ./scripts/build_qemu_binary.sh >/dev/null
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
