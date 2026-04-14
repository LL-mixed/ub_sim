#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
INITRAMFS_DIR="$OUT_DIR/initramfs"
PROBE_SRC="$ROOT_DIR/probe.c"
PROBE_BIN="$OUT_DIR/linqu_probe"
URMA_DP_SRC="$ROOT_DIR/urma_dp.c"
URMA_DP_BIN="$OUT_DIR/linqu_urma_dp"
INIT_SRC="$ROOT_DIR/init.c"
INIT_BIN="$OUT_DIR/init"
INSMOD_SRC="$ROOT_DIR/insmod.c"
INSMOD_BIN="$OUT_DIR/insmod"
INIT_MANUAL_BIND_SRC="$ROOT_DIR/init_manual_bind.c"
INIT_MANUAL_BIND_BIN="$OUT_DIR/init_manual_bind"
CHAT_SRC="$ROOT_DIR/ub_chat.c"
CHAT_BIN="$OUT_DIR/linqu_ub_chat"
RPC_SRC="$ROOT_DIR/ub_rpc_demo.c"
RPC_BIN="$OUT_DIR/linqu_ub_rpc"
TCP_EACH_SERVER_SRC="$ROOT_DIR/ub_tcp_each_server_demo.c"
TCP_EACH_SERVER_BIN="$OUT_DIR/linqu_ub_tcp_each_server"
RDMA_SRC="$ROOT_DIR/ub_rdma_demo.c"
RDMA_BIN="$OUT_DIR/linqu_ub_rdma_demo"
OBMM_SRC="$ROOT_DIR/ub_obmm_demo.c"
OBMM_BIN="$OUT_DIR/linqu_ub_obmm_demo"
RUN_DEMO_SRC="$ROOT_DIR/initramfs/run_demo"
RUN_DEMO_BIN="$INITRAMFS_DIR/bin/run_demo"
INIT_SCRIPT_SRC="$ROOT_DIR/initramfs/init"
INIT_SCRIPT_BIN="$INITRAMFS_DIR/init"
LINQU_INIT_BIN="$INITRAMFS_DIR/bin/linqu_init"
RDINIT_INTERACTIVE_SRC="$ROOT_DIR/initramfs/rdinit_interactive"
RDINIT_INTERACTIVE_BIN="$INITRAMFS_DIR/bin/rdinit_interactive"
INIT_BIN_TO_USE="${INIT_TO_USE:-$INIT_BIN}"
INITRAMFS_IMG="$OUT_DIR/initramfs.cpio.gz"

LINQU_MODULE="${LINQU_UB_GUEST_MODULE:-}"
HISI_UBUS_MODULE="${HISI_UBUS_GUEST_MODULE:-}"
UBUS_MODULE="${UB_UBUS_GUEST_MODULE:-}"
UB_SIM_DECODER_MODULE="${UB_SIM_DECODER_GUEST_MODULE:-}"
OBMM_MODULE="${UB_OBMM_GUEST_MODULE:-}"
UBASE_MODULE="${UB_UBASE_GUEST_MODULE:-}"
UBCORE_MODULE="${UB_UBCORE_GUEST_MODULE:-}"
UDMA_MODULE="${UB_UDMA_GUEST_MODULE:-}"
IPOURMA_MODULE="${UB_IPOURMA_GUEST_MODULE:-}"
UBURMA_MODULE="${UB_URMA_GUEST_MODULE:-}"
UMMU_CORE_MODULE="${UB_UMMU_CORE_GUEST_MODULE:-}"
UMMU_MODULE="${UB_UMMU_GUEST_MODULE:-}"

COPY_ALL_KO="${COPY_ALL_KO:-0}"
ALLOW_OUT_DIR_MODULE_FALLBACK="${ALLOW_OUT_DIR_MODULE_FALLBACK:-0}"

: "${AARCH64_LINUX_CC:=}"
: "${BUSYBOX:=}"
if [[ -z "$BUSYBOX" ]] && [[ -x "$ROOT_DIR/busybox-aarch64" ]]; then
  BUSYBOX="$ROOT_DIR/busybox-aarch64"
fi

detect_make_jobs() {
  getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8
}

ensure_busybox_static_config() {
  local src_dir="$1"
  local cc_path="$2"
  local cc_name=""
  local cc_prefix=""

  cc_name="$(basename "$cc_path")"
  cc_prefix="${cc_name%gcc}"
  if [[ -z "$cc_prefix" || "$cc_prefix" == "$cc_name" ]]; then
    echo "[build_initramfs] error: cannot derive CROSS_COMPILER_PREFIX from $cc_path" >&2
    return 1
  fi

  make -C "$src_dir" defconfig >/dev/null

  perl -0pi -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/m' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_STATIC=.*$/CONFIG_STATIC=y/m' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_CROSS_COMPILER_PREFIX=.*\n//mg' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_EXTRA_CFLAGS=.*\n//mg' "$src_dir/.config"
  printf 'CONFIG_CROSS_COMPILER_PREFIX="%s"\n' "$cc_prefix" >> "$src_dir/.config"
  printf 'CONFIG_EXTRA_CFLAGS="-static"\n' >> "$src_dir/.config"
}

build_busybox_from_source_dir() {
  local src_dir="$1"
  local out_bin="$2"
  local cc_path="$3"
  local jobs

  jobs="$(detect_make_jobs)"
  echo "[build_initramfs] building busybox from source: $src_dir" >&2

  ensure_busybox_static_config "$src_dir" "$cc_path"
  make -C "$src_dir" -j"$jobs" >/dev/null

  if [[ ! -x "$src_dir/busybox" ]]; then
    echo "[build_initramfs] error: busybox build did not produce $src_dir/busybox" >&2
    return 1
  fi

  cp "$src_dir/busybox" "$out_bin"
  chmod +x "$out_bin"
}

ensure_busybox_binary() {
  local third_party_dir="$ROOT_DIR/third_party"
  local local_bin="$ROOT_DIR/busybox-aarch64"
  local third_party_bin="$third_party_dir/busybox-aarch64"
  local src_dir="$third_party_dir/busybox-src"
  local extracted_dir=""
  local tarball=""

  if [[ -n "$BUSYBOX" ]]; then
    if [[ ! -x "$BUSYBOX" ]]; then
      echo "[build_initramfs] error: BUSYBOX is set but not executable: $BUSYBOX" >&2
      return 1
    fi
    return 0
  fi

  if [[ -x "$local_bin" ]]; then
    BUSYBOX="$local_bin"
    return 0
  fi

  if [[ -x "$third_party_bin" ]]; then
    cp "$third_party_bin" "$local_bin"
    chmod +x "$local_bin"
    BUSYBOX="$local_bin"
    return 0
  fi

  if [[ -z "$AARCH64_LINUX_CC" ]]; then
    echo "[build_initramfs] error: AARCH64_LINUX_CC is required to build busybox" >&2
    return 1
  fi

  mkdir -p "$third_party_dir"

  if [[ -d "$src_dir" ]]; then
    build_busybox_from_source_dir "$src_dir" "$local_bin" "$AARCH64_LINUX_CC"
    BUSYBOX="$local_bin"
    return 0
  fi

  tarball="$(find "$third_party_dir" -maxdepth 1 -type f -name 'busybox-*.tar.bz2' | head -n 1)"
  if [[ -n "$tarball" ]]; then
    echo "[build_initramfs] extracting busybox source from $tarball" >&2
    tar -xf "$tarball" -C "$third_party_dir"
    extracted_dir="$(find "$third_party_dir" -maxdepth 1 -type d -name 'busybox-*' ! -name 'busybox-src' | head -n 1)"
    if [[ -z "$extracted_dir" ]]; then
      echo "[build_initramfs] error: failed to locate extracted busybox source under $third_party_dir" >&2
      return 1
    fi
    rm -rf "$src_dir"
    mv "$extracted_dir" "$src_dir"
    build_busybox_from_source_dir "$src_dir" "$local_bin" "$AARCH64_LINUX_CC"
    BUSYBOX="$local_bin"
    return 0
  fi

  echo "[build_initramfs] error: missing ARM64 busybox binary and no local busybox source/tarball available" >&2
  echo "[build_initramfs] expected one of:" >&2
  echo "[build_initramfs]   - BUSYBOX=/path/to/busybox-aarch64" >&2
  echo "[build_initramfs]   - $ROOT_DIR/busybox-aarch64" >&2
  echo "[build_initramfs]   - $third_party_bin" >&2
  echo "[build_initramfs]   - $src_dir" >&2
  echo "[build_initramfs]   - $third_party_dir/busybox-*.tar.bz2" >&2
  return 1
}

resolve_module_path() {
  local explicit_path="$1"
  local module_name="$2"
  local candidate=""

  if [[ -n "$explicit_path" ]]; then
    echo "$explicit_path"
    return 0
  fi

  candidate="$OUT_DIR/modules/$module_name"
  if [[ -f "$candidate" ]]; then
    echo "$candidate"
    return 0
  fi

  if [[ "$ALLOW_OUT_DIR_MODULE_FALLBACK" == "1" ]]; then
    candidate="$OUT_DIR/$module_name"
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  fi

  echo ""
}

copy_module_if_present() {
  local src="$1"
  local dst_name="$2"
  local required="$3"
  local resolved=""

  resolved="$(resolve_module_path "$src" "$dst_name")"
  if [[ -z "$resolved" ]]; then
    if [[ "$required" == "1" ]]; then
      echo "[build_initramfs] warn: missing required module $dst_name" >&2
    fi
    return 0
  fi

  if [[ ! -f "$resolved" ]]; then
    echo "[build_initramfs] warn: module path not found: $resolved" >&2
    return 0
  fi

  cp "$resolved" "$INITRAMFS_DIR/lib/modules/$dst_name"
}

link_busybox_applet() {
  local applet="$1"
  ln -sf busybox "$INITRAMFS_DIR/bin/$applet"
}

mkdir -p "$OUT_DIR"
rm -rf "$INITRAMFS_DIR"
mkdir -p \
  "$INITRAMFS_DIR/bin" \
  "$INITRAMFS_DIR/dev" \
  "$INITRAMFS_DIR/proc" \
  "$INITRAMFS_DIR/sys" \
  "$INITRAMFS_DIR/tmp" \
  "$INITRAMFS_DIR/lib/modules"

if [[ -z "$AARCH64_LINUX_CC" ]]; then
  echo "AARCH64_LINUX_CC is required" >&2
  echo "example: export AARCH64_LINUX_CC=aarch64-linux-gnu-gcc" >&2
  exit 1
fi

ensure_busybox_binary

"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$PROBE_SRC" -o "$PROBE_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$URMA_DP_SRC" -o "$URMA_DP_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INIT_SRC" -o "$INIT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INSMOD_SRC" -o "$INSMOD_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INIT_MANUAL_BIND_SRC" -o "$INIT_MANUAL_BIND_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$CHAT_SRC" -o "$CHAT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$RPC_SRC" -o "$RPC_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$TCP_EACH_SERVER_SRC" -o "$TCP_EACH_SERVER_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$RDMA_SRC" -o "$RDMA_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$OBMM_SRC" -o "$OBMM_BIN"

if [[ -f "$INIT_SCRIPT_SRC" ]]; then
  cp "$INIT_SCRIPT_SRC" "$INIT_SCRIPT_BIN"
  chmod +x "$INIT_SCRIPT_BIN"
else
  echo "[build_initramfs] error: missing init script template: $INIT_SCRIPT_SRC" >&2
  exit 1
fi

cp "$INIT_BIN_TO_USE" "$LINQU_INIT_BIN"
chmod +x "$LINQU_INIT_BIN"
cp "$PROBE_BIN" "$INITRAMFS_DIR/bin/linqu_probe"
cp "$URMA_DP_BIN" "$INITRAMFS_DIR/bin/linqu_urma_dp"
cp "$INSMOD_BIN" "$INITRAMFS_DIR/bin/insmod"
cp "$CHAT_BIN" "$INITRAMFS_DIR/bin/linqu_ub_chat"
cp "$RPC_BIN" "$INITRAMFS_DIR/bin/linqu_ub_rpc"
cp "$TCP_EACH_SERVER_BIN" "$INITRAMFS_DIR/bin/linqu_ub_tcp_each_server"
cp "$RDMA_BIN" "$INITRAMFS_DIR/bin/linqu_ub_rdma_demo"
cp "$OBMM_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_demo"

cp "$BUSYBOX" "$INITRAMFS_DIR/bin/busybox"
chmod +x "$INITRAMFS_DIR/bin/busybox"
link_busybox_applet sh
link_busybox_applet ls
link_busybox_applet mount
link_busybox_applet mkdir
link_busybox_applet cat
link_busybox_applet sleep
link_busybox_applet dmesg
link_busybox_applet head
link_busybox_applet tail
link_busybox_applet grep
link_busybox_applet ps
link_busybox_applet uname
link_busybox_applet ifconfig
link_busybox_applet route
link_busybox_applet netstat
link_busybox_applet ip
link_busybox_applet arp
link_busybox_applet ping
link_busybox_applet ping6

if [[ -f "$RUN_DEMO_SRC" ]]; then
  cp "$RUN_DEMO_SRC" "$RUN_DEMO_BIN"
  chmod +x "$RUN_DEMO_BIN"
else
  echo "[build_initramfs] warn: missing run_demo script template: $RUN_DEMO_SRC" >&2
fi

if [[ -f "$RDINIT_INTERACTIVE_SRC" ]]; then
  cp "$RDINIT_INTERACTIVE_SRC" "$RDINIT_INTERACTIVE_BIN"
  chmod +x "$RDINIT_INTERACTIVE_BIN"
else
  echo "[build_initramfs] warn: missing interactive rdinit template: $RDINIT_INTERACTIVE_SRC" >&2
fi

if [[ "$COPY_ALL_KO" == "1" ]] && [[ -d "$OUT_DIR" ]]; then
  for ko_file in "$OUT_DIR"/*.ko; do
    if [[ -f "$ko_file" ]]; then
      cp "$ko_file" "$INITRAMFS_DIR/lib/modules/"
    fi
  done
fi

copy_module_if_present "$LINQU_MODULE" "linqu_ub_drv.ko" 0
copy_module_if_present "$HISI_UBUS_MODULE" "hisi_ubus.ko" 0
copy_module_if_present "$UBUS_MODULE" "ubus.ko" 0
copy_module_if_present "$UB_SIM_DECODER_MODULE" "ub-sim-decoder.ko" 0
copy_module_if_present "$OBMM_MODULE" "obmm.ko" 0
copy_module_if_present "$UBASE_MODULE" "ubase.ko" 0
copy_module_if_present "$UBCORE_MODULE" "ubcore.ko" 0
copy_module_if_present "$UDMA_MODULE" "udma.ko" 0
copy_module_if_present "$IPOURMA_MODULE" "ipourma.ko" 0
copy_module_if_present "$UBURMA_MODULE" "uburma.ko" 0
copy_module_if_present "$UMMU_CORE_MODULE" "ummu-core.ko" 0
copy_module_if_present "$UMMU_MODULE" "ummu.ko" 0

echo "[build_initramfs] packaged modules:"
ls -1 "$INITRAMFS_DIR/lib/modules" | sed 's/^/[build_initramfs]   /'

(
  cd "$INITRAMFS_DIR"
  printf 'console\0' | cpio -o --null -H newc --quiet > /dev/null 2>&1 || true
)

(
  cd "$INITRAMFS_DIR"
  find . -print | cpio -o -H newc --quiet | gzip -9 > "$INITRAMFS_IMG"
)

echo "$INITRAMFS_IMG"
echo "built $INITRAMFS_IMG"
