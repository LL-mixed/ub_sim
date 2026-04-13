#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
THIRD_PARTY_DIR="$ROOT_DIR/third_party"
OUT_BIN="$ROOT_DIR/busybox-aarch64"
THIRD_PARTY_BIN="$THIRD_PARTY_DIR/busybox-aarch64"
SRC_DIR="$THIRD_PARTY_DIR/busybox-src"
BUSYBOX_VERSION="${BUSYBOX_VERSION:-1.36.1}"
BUSYBOX_TARBALL="$THIRD_PARTY_DIR/busybox-${BUSYBOX_VERSION}.tar.bz2"
BUSYBOX_URL="${BUSYBOX_URL:-https://busybox.net/downloads/busybox-${BUSYBOX_VERSION}.tar.bz2}"
VM_HOST="${VM_HOST:-ll@192.168.64.3}"
VM_BUSYBOX_PATH="${VM_BUSYBOX_PATH:-/usr/bin/busybox_aarch64}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

CC="${AARCH64_LINUX_CC:-$(detect_aarch64_linux_cc)}"

detect_jobs() {
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
    echo "[prepare_busybox] error: cannot derive CROSS_COMPILER_PREFIX from $cc_path" >&2
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

build_from_source_dir() {
  local src_dir="$1"
  local jobs
  jobs="$(detect_jobs)"

  if [[ -z "$CC" ]]; then
    echo "[prepare_busybox] error: AARCH64_LINUX_CC is required to build busybox from source" >&2
    return 1
  fi

  echo "[prepare_busybox] building busybox from source: $src_dir" >&2
  ensure_busybox_static_config "$src_dir" "$CC"
  make -C "$src_dir" -j"$jobs" >/dev/null

  if [[ ! -x "$src_dir/busybox" ]]; then
    echo "[prepare_busybox] error: build did not produce $src_dir/busybox" >&2
    return 1
  fi

  cp "$src_dir/busybox" "$OUT_BIN"
  chmod +x "$OUT_BIN"
}

download_busybox_tarball() {
  if [[ -f "$BUSYBOX_TARBALL" ]]; then
    return 0
  fi

  echo "[prepare_busybox] downloading busybox source: $BUSYBOX_URL" >&2
  if command -v curl >/dev/null 2>&1; then
    curl -L "$BUSYBOX_URL" -o "$BUSYBOX_TARBALL"
    return 0
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -O "$BUSYBOX_TARBALL" "$BUSYBOX_URL"
    return 0
  fi
  return 1
}

if [[ -n "${BUSYBOX:-}" && -x "${BUSYBOX:-}" ]]; then
  echo "$BUSYBOX"
  exit 0
fi

if [[ -x "$OUT_BIN" ]]; then
  echo "$OUT_BIN"
  exit 0
fi

mkdir -p "$THIRD_PARTY_DIR"

if [[ -x "$THIRD_PARTY_BIN" ]]; then
  cp "$THIRD_PARTY_BIN" "$OUT_BIN"
  chmod +x "$OUT_BIN"
  echo "$OUT_BIN"
  exit 0
fi

if [[ -d "$SRC_DIR" ]]; then
  build_from_source_dir "$SRC_DIR"
  echo "$OUT_BIN"
  exit 0
fi

tarball="$(find "$THIRD_PARTY_DIR" -maxdepth 1 -type f -name 'busybox-*.tar.bz2' | head -n 1)"
if [[ -z "${tarball:-}" ]] && download_busybox_tarball; then
  tarball="$BUSYBOX_TARBALL"
fi
if [[ -n "${tarball:-}" ]]; then
  echo "[prepare_busybox] extracting busybox source from $tarball" >&2
  tar -xf "$tarball" -C "$THIRD_PARTY_DIR"
  extracted_dir="$(find "$THIRD_PARTY_DIR" -maxdepth 1 -type d -name 'busybox-*' ! -name 'busybox-src' | head -n 1)"
  if [[ -z "$extracted_dir" ]]; then
    echo "[prepare_busybox] error: failed to locate extracted busybox source under $THIRD_PARTY_DIR" >&2
    exit 1
  fi
  rm -rf "$SRC_DIR"
  mv "$extracted_dir" "$SRC_DIR"
  build_from_source_dir "$SRC_DIR"
  echo "$OUT_BIN"
  exit 0
fi

if ssh -o BatchMode=yes -o ConnectTimeout=5 "$VM_HOST" "test -x '$VM_BUSYBOX_PATH'" >/dev/null 2>&1; then
  echo "[prepare_busybox] copying busybox from VM: $VM_HOST:$VM_BUSYBOX_PATH" >&2
  scp "$VM_HOST:$VM_BUSYBOX_PATH" "$OUT_BIN" >/dev/null
  chmod +x "$OUT_BIN"
  echo "$OUT_BIN"
  exit 0
fi

cat >&2 <<EOF
[prepare_busybox] error: unable to prepare ARM64 busybox
[prepare_busybox] expected one of:
[prepare_busybox]   - BUSYBOX=/path/to/busybox-aarch64
[prepare_busybox]   - $OUT_BIN
[prepare_busybox]   - $THIRD_PARTY_BIN
[prepare_busybox]   - $SRC_DIR
[prepare_busybox]   - $THIRD_PARTY_DIR/busybox-*.tar.bz2
[prepare_busybox]   - automatic download via curl/wget from $BUSYBOX_URL
[prepare_busybox]   - reachable VM with $VM_BUSYBOX_PATH
EOF
exit 1
