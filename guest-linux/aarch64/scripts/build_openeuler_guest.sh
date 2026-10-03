#!/bin/zsh
# build_openeuler_guest.sh — produce a qcow2 W5 guest rootfs based on openEuler,
# as an alternative to build_initramfs.sh's busybox+cpio path. Selectable at
# W5 run time via SIM_W5_GUEST_BACKEND=openeuler.
#
# Inputs:
#   --kernel-image <path>   default: guest-linux/aarch64/out/Image
#   --modules-dir <dir>     default: guest-linux/aarch64/out/modules (the .ko files)
#   --tools-dir <dir>       default: guest-linux/aarch64/out/guest_tools (the -static C tools)
#   --rootfs-source <kind>  docker | download | dnf  (default: docker)
#   --docker-image <ref>    default: openeuler-2403:v0.0.4
#   --rootfs-url <url>      for download source; auto-picks official if empty
#   --output-dir <dir>      default: guest-linux/aarch64/out/openeuler_guest
#   --rootfs-size-gb <n>    default: 32 (qcow2 virtual size; the openEuler
#                           2403 image unpacks to ~22GB, so 32 leaves headroom)
#   --keep-tmp              don't scrub the intermediate tar/scratch dir
#
# Outputs (under --output-dir):
#   rootfs.qcow2            base image, read-only by guests (per-node overlays sit on top)
#   kernel.img -> <kernel-image>
#   modules/                .ko files (also baked into rootfs; this copy is for 9p fallback)
#   tools/                  -static C tools (also baked into rootfs; 9p fallback)
#   env/                    per-run env injection point (9p mount source)
#   run_openeuler_guest.sh  single-node QEMU launcher, called by run_llm_infer_eight_node_guest.sh
#   .build_signature       hash of inputs to skip rebuild on second run
#
# Notes:
#   - Does NOT touch the existing cpio+rdinit pipeline. Selecting
#     SIM_W5_GUEST_BACKEND=openeuler in the W5 cluster scripts switches the
#     runtime; default remains busybox.
#   - Requires: docker (for docker/dnf source), qemu-img, curl/xz (for download source).
#   - Host arch is verified — kernel_ub is aarch64 only.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_PATH="$(cd "$SCRIPT_DIR" && pwd)/$(basename "$0")"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
OUT_ROOT="$ROOT_DIR/out"
DEFAULT_KERNEL_IMAGE="$OUT_ROOT/Image"
DEFAULT_MODULES_DIR="$OUT_ROOT/modules"
DEFAULT_TOOLS_DIR="$OUT_ROOT/guest_tools"
DEFAULT_OUTPUT_DIR="$OUT_ROOT/openeuler_guest"
DEFAULT_DOCKER_IMAGE="openeuler-2403:v0.0.4"
DEFAULT_ROOTFS_SIZE_GB=32

KERNEL_IMAGE="$DEFAULT_KERNEL_IMAGE"
MODULES_DIR="$DEFAULT_MODULES_DIR"
TOOLS_DIR="$DEFAULT_TOOLS_DIR"
ROOTFS_SOURCE="docker"
DOCKER_IMAGE="$DEFAULT_DOCKER_IMAGE"
ROOTFS_URL=""
OUTPUT_DIR="$DEFAULT_OUTPUT_DIR"
ROOTFS_SIZE_GB="$DEFAULT_ROOTFS_SIZE_GB"
KEEP_TMP=0

usage() {
  cat >&2 <<'USAGE'
usage: build_openeuler_guest.sh [options]

  --kernel-image <path>    kernel_ub ARM64 Image (default: out/Image)
  --modules-dir <dir>      directory of .ko files (default: out/modules)
  --tools-dir <dir>        directory of -static C tools (default: out/guest_tools)
  --rootfs-source <kind>   docker | download | dnf (default: docker)
  --docker-image <ref>     docker image ref for source=docker (default: openeuler-2403:v0.0.4)
  --rootfs-url <url>       override URL for source=download
  --output-dir <dir>       default: out/openeuler_guest
  --rootfs-size-gb <n>     qcow2 virtual size (default: 32)
  --keep-tmp               keep intermediate scratch dir for debugging
  -h, --help
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --kernel-image) KERNEL_IMAGE="$2"; shift 2 ;;
    --modules-dir) MODULES_DIR="$2"; shift 2 ;;
    --tools-dir) TOOLS_DIR="$2"; shift 2 ;;
    --rootfs-source) ROOTFS_SOURCE="$2"; shift 2 ;;
    --docker-image) DOCKER_IMAGE="$2"; shift 2 ;;
    --rootfs-url) ROOTFS_URL="$2"; shift 2 ;;
    --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
    --rootfs-size-gb) ROOTFS_SIZE_GB="$2"; shift 2 ;;
    --keep-tmp) KEEP_TMP=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; usage; exit 2 ;;
  esac
done

log() { echo "[build_openeuler_guest] $*" >&2; }
die() { echo "[build_openeuler_guest] error: $*" >&2; exit 1; }

need_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || die "missing required command on PATH: $cmd"
}

have_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1
}

# qemu-img is optional on the host; we install it in-container if missing.
have_cmd qemu-img || log "host lacks qemu-img; will install in-container via dnf"
case "$ROOTFS_SOURCE" in
  docker|dnf) need_cmd docker ;;
  download) need_cmd curl; need_cmd xz ;;
  *) die "unknown --rootfs-source: $ROOTFS_SOURCE (use docker|download|dnf)" ;;
esac

[[ -f "$KERNEL_IMAGE" ]] || die "kernel image missing: $KERNEL_IMAGE (run sync_ub_kernel_artifacts_from_remote_linux.sh first)"

if [[ "$(uname -m)" != "aarch64" ]]; then
  die "host arch is $(uname -m); build_openeuler_guest.sh requires aarch64 (kernel_ub is aarch64-only)"
fi

mkdir -p "$OUTPUT_DIR"
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"
TMP_DIR="$OUTPUT_DIR/.tmp.$$"
mkdir -p "$TMP_DIR"

trap '$([[ $KEEP_TMP == 1 ]] || rm -rf "$TMP_DIR")' EXIT

# ---- build initramfs (init_switch_root) for modular virtio ------------------
#
# openeuler_defconfig has CONFIG_VIRTIO_BLK=m, CONFIG_VIRTIO_PCI=m,
# CONFIG_VIRTIO_MMIO=m, CONFIG_9P_FS=m — modular. The kernel can't see
# /dev/vda or mount 9p without an initramfs that insmods the virtio modules
# first. This builder reuses the busybox+init_switch_root pattern from
# guest-linux/aarch64/initramfs/init_switch_root but simplifies it for our
# direct-ext4-on-/dev/vda layout (no LVM).
#
# Inputs (from the kernel_build dir):
#   drivers/virtio/virtio.ko virtio_pci.ko virtio_mmio.ko
#   drivers/block/virtio_blk.ko
#   net/9p/9pnet.ko 9pnet_virtio.ko
#   fs/9p/9p.ko
#
# Output: $OUTPUT_DIR/initramfs.cpio.gz
build_openeuler_initramfs() {
  local kernel_build="$ROOT_DIR/out/kernel_build"
  [[ -d "$kernel_build" ]] || kernel_build="$ROOT_DIR/out"
  local busybox_bin="$ROOT_DIR/busybox-aarch64"
  [[ -f "$busybox_bin" ]] || busybox_bin="$REPO_ROOT/guest-linux/aarch64/busybox-aarch64"
  [[ -f "$busybox_bin" ]] || die "busybox-aarch64 not found (looked in $ROOT_DIR and $REPO_ROOT/guest-linux/aarch64)"

  local initramfs_root="$TMP_DIR/initramfs"
  mkdir -p "$initramfs_root/bin" "$initramfs_root/lib/modules" \
           "$initramfs_root/proc" "$initramfs_root/sys" "$initramfs_root/dev" \
           "$initramfs_root/newroot" "$initramfs_root/tmp"

  # busybox + applet symlinks (init_switch_root uses /bin/busybox <cmd> form)
  cp -a "$busybox_bin" "$initramfs_root/bin/busybox"
  chmod +x "$initramfs_root/bin/busybox"
  local applet
  for applet in sh mkdir mount insmod sleep ls cat sed awk cp rm basename \
                readlink switch_root mknod echo; do
    ln -sf busybox "$initramfs_root/bin/$applet"
  done

  # virtio + ext4 + 9p modules. Load order matters — each module's deps must
  # be loaded before it. The full transitive closure (modinfo verified):
  #   virtio_ring (no deps)
  #   virtio → virtio_ring
  #   virtio_pci_modern_dev, virtio_pci_legacy_dev (no deps)
  #   virtio_pci → virtio_ring, virtio_pci_modern_dev, virtio_pci_legacy_dev, virtio
  #   virtio_mmio → virtio, virtio_ring
  #   virtio_blk → virtio, virtio_ring
  #   mbcache, jbd2 (no deps)
  #   ext4 → jbd2, mbcache
  #   netfs (no deps)
  #   fscache → netfs
  #   9pnet (no deps)
  #   9pnet_virtio → virtio, 9pnet, virtio_ring
  #   9p → 9pnet, fscache, netfs
  local -a wanted_modules=(
    "drivers/virtio/virtio_ring.ko"
    "drivers/virtio/virtio.ko"
    "drivers/virtio/virtio_pci_modern_dev.ko"
    "drivers/virtio/virtio_pci_legacy_dev.ko"
    "drivers/virtio/virtio_pci.ko"
    "drivers/virtio/virtio_mmio.ko"
    "drivers/block/virtio_blk.ko"
    "fs/mbcache.ko"
    "fs/jbd2/jbd2.ko"
    "fs/ext4/ext4.ko"
    "fs/netfs/netfs.ko"
    "fs/fscache/fscache.ko"
    "net/9p/9pnet.ko"
    "net/9p/9pnet_virtio.ko"
    "fs/9p/9p.ko"
  )
  local mod_path mod_name config_key
  local -A required_9p_configs=(
    netfs.ko CONFIG_NETFS_SUPPORT
    fscache.ko CONFIG_FSCACHE
    9pnet.ko CONFIG_NET_9P
    9pnet_virtio.ko CONFIG_NET_9P_VIRTIO
    9p.ko CONFIG_9P_FS
  )
  for mod_path in "${wanted_modules[@]}"; do
    mod_name="$(basename "$mod_path")"
    if [[ -f "$kernel_build/$mod_path" ]]; then
      cp -a "$kernel_build/$mod_path" "$initramfs_root/lib/modules/$mod_name"
      log "  initramfs: $mod_name"
    elif [[ -n "${required_9p_configs[$mod_name]:-}" ]]; then
      config_key="${required_9p_configs[$mod_name]}"
      if [[ -f "$kernel_build/.config" ]] && grep -q "^${config_key}=y$" "$kernel_build/.config"; then
        log "  initramfs: $mod_name is built into the kernel"
      else
        die "required openEuler 9p module missing: $mod_path (run build_guest_artifacts.sh)"
      fi
    else
      log "  initramfs: WARNING missing $mod_path"
    fi
  done

  # init script — adapted from initramfs/init_switch_root but simplified for
  # direct ext4 on /dev/vda (no LVM, no UB device bind).
  cat >"$initramfs_root/init" <<'INIT'
#!/bin/busybox sh
set -e

log() { echo "[oe-init] $*"; }
die() {
  echo "[oe-init] FATAL: $*" >&2
  echo "[oe-init] dropping to shell..." >&2
  exec /bin/busybox sh
}

mount_fs() {
  fstype="$1"
  target="$2"
  /bin/busybox mkdir -p "$target" >/dev/null 2>&1 || true
  /bin/busybox mount -t "$fstype" none "$target" >/dev/null 2>&1 || true
}

# 1. Mount basic filesystems
mount_fs proc /proc
mount_fs sysfs /sys
mount_fs devtmpfs /dev
/bin/busybox mkdir -p /dev/pts /tmp
/bin/busybox mount -t devpts devpts /dev/pts >/dev/null 2>&1 || true

# 2. Load virtio + ext4 + 9p modules (modular in openeuler_defconfig)
# Load order matters — deps before users. See build script comment for the
# transitive closure.
log "loading virtio + ext4 + 9p modules..."
for mod in virtio_ring virtio virtio_pci_modern_dev virtio_pci_legacy_dev \
           virtio_pci virtio_mmio virtio_blk mbcache jbd2 ext4 netfs fscache \
           9pnet 9pnet_virtio 9p; do
  ko="/lib/modules/${mod}.ko"
  if [ -f "$ko" ]; then
    if /bin/busybox insmod "$ko" 2>/dev/null; then
      log "  loaded ${mod}"
    else
      log "  skip ${mod} (maybe built-in or failed)"
    fi
  fi
done

# 3. Wait for virtio-blk to enumerate /dev/vda
log "waiting for /dev/vda..."
attempt=0
max_attempts=30
while [ $attempt -lt $max_attempts ]; do
  if [ -b /dev/vda ]; then
    log "  /dev/vda ready"
    break
  fi
  /bin/busybox sleep 1
  attempt=$((attempt + 1))
done
if [ $attempt -eq $max_attempts ]; then
  die "virtio-blk /dev/vda never appeared"
fi

# 4. Mount rootfs (direct ext4 — no LVM in our build)
log "mounting rootfs /dev/vda..."
/bin/busybox mkdir -p /newroot
/bin/busybox mount -t ext4 -o rw /dev/vda /newroot || die "failed to mount /dev/vda"

# 5. Sanitize /etc/fstab — comment out /boot/efi and swap entries that would
#    otherwise cause systemd to drop into emergency mode.
if [ -f /newroot/etc/fstab ]; then
  log "sanitizing /etc/fstab..."
  /bin/busybox cp /newroot/etc/fstab /newroot/etc/fstab.oe_bak
  /bin/busybox awk '
    /^[[:space:]]*#/ { print; next }
    /[[:space:]]\/boot\/efi[[:space:]]/ { print "#oe-sim# " $0; next }
    /[[:space:]]none[[:space:]]+swap[[:space:]]/ { print "#oe-sim# " $0; next }
    /[[:space:]]swap[[:space:]]+swap[[:space:]]/ { print "#oe-sim# " $0; next }
    { print }
  ' /newroot/etc/fstab.oe_bak > /newroot/etc/fstab
  /bin/busybox rm -f /newroot/etc/fstab.oe_bak
fi

# 6. Move mounts into /newroot
/bin/busybox mkdir -p /newroot/proc /newroot/sys /newroot/dev /newroot/run
/bin/busybox mount --move /proc /newroot/proc 2>/dev/null || \
  /bin/busybox mount -t proc proc /newroot/proc
/bin/busybox mount --move /sys /newroot/sys 2>/dev/null || \
  /bin/busybox mount -t sysfs sysfs /newroot/sys
/bin/busybox mount --move /dev /newroot/dev 2>/dev/null || \
  /bin/busybox mount -t devtmpfs devtmpfs /newroot/dev

# 7. switch_root to systemd
log "switch_root -> /sbin/init"
exec /bin/busybox switch_root /newroot /sbin/init
die "switch_root returned"
INIT
  chmod +x "$initramfs_root/init"

  # Pack cpio.gz
  local initramfs_img="$OUTPUT_DIR/initramfs.cpio.gz"
  ( cd "$initramfs_root" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -9 >"$initramfs_img" )
  log "initramfs: $initramfs_img ($(du -h "$initramfs_img" | cut -f1))"
}

# ---- signature: skip rebuild if inputs unchanged ---------------------------

sig_file="$OUTPUT_DIR/.build_signature"
new_sig=""
hash_cmd=""
if command -v sha256sum >/dev/null 2>&1; then
  hash_cmd="sha256sum"
elif command -v shasum >/dev/null 2>&1; then
  hash_cmd="shasum -a 256"
else
  die "no sha256/shasum on PATH"
fi

signature_inputs() {
  echo "kernel_image=$KERNEL_IMAGE"
  [[ -f "$KERNEL_IMAGE" ]] && $hash_cmd "$KERNEL_IMAGE"
  echo "modules_dir=$MODULES_DIR"
  if [[ -d "$MODULES_DIR" ]]; then
    find "$MODULES_DIR" -type f -name '*.ko' | sort | xargs -r $hash_cmd
  fi
  echo "tools_dir=$TOOLS_DIR"
  if [[ -d "$TOOLS_DIR" ]]; then
    find "$TOOLS_DIR" -type f -executable | sort | xargs -r $hash_cmd
  fi
  echo "rootfs_source=$ROOTFS_SOURCE"
  echo "docker_image=$DOCKER_IMAGE"
  echo "rootfs_url=$ROOTFS_URL"
  echo "rootfs_size_gb=$ROOTFS_SIZE_GB"
  $hash_cmd "$SCRIPT_PATH"
}

new_sig="$(signature_inputs | $hash_cmd)"
if [[ -f "$sig_file" && "$(cat "$sig_file")" == "$new_sig" && -f "$OUTPUT_DIR/rootfs.qcow2" ]]; then
  log "inputs unchanged, reusing $OUTPUT_DIR/rootfs.qcow2 (set --keep-tmp to force)"
  # Still refresh the kernel.img symlink and 9p-shared mounts in case paths moved.
  ln -sf "$KERNEL_IMAGE" "$OUTPUT_DIR/kernel.img"
  mkdir -p "$OUTPUT_DIR/modules" "$OUTPUT_DIR/tools" "$OUTPUT_DIR/env"
  [[ -d "$MODULES_DIR" ]] && rsync -a --delete "$MODULES_DIR/" "$OUTPUT_DIR/modules/" 2>/dev/null || true
  [[ -d "$TOOLS_DIR" ]] && rsync -a --delete "$TOOLS_DIR/" "$OUTPUT_DIR/tools/" 2>/dev/null || true
  # initramfs depends on kernel_build modules + busybox; rebuild if missing.
  if [[ ! -f "$OUTPUT_DIR/initramfs.cpio.gz" ]]; then
    build_openeuler_initramfs
  fi
  exit 0
fi

# ---- rootfs acquisition ----------------------------------------------------

ROOTFS_TAR="$TMP_DIR/rootfs.tar"
ROOTFS_QCOW2_RAW="$TMP_DIR/rootfs.raw"
ROOTFS_QCOW2="$OUTPUT_DIR/rootfs.qcow2"

case "$ROOTFS_SOURCE" in
  docker)
    log "acquiring rootfs from docker image: $DOCKER_IMAGE"
    cid="$(docker create --platform linux/arm64 "$DOCKER_IMAGE" /bin/true)"
    docker export "$cid" >"$ROOTFS_TAR"
    docker rm "$cid" >/dev/null
    ;;
  download)
    if [[ -z "$ROOTFS_URL" ]]; then
      # Official openEuler 2403 LTS aarch64 rootfs. Pinned URL — update when version bumps.
      ROOTFS_URL="https://repo.openeuler.org/openEuler-24.03-LTS/OS/aarch64/images/rootfs.qcow2.xz"
    fi
    log "downloading: $ROOTFS_URL"
    curl -fL --retry 3 -o "$TMP_DIR/rootfs.qcow2.xz" "$ROOTFS_URL"
    xz -d "$TMP_DIR/rootfs.qcow2.xz"
    mv "$TMP_DIR/rootfs.qcow2" "$ROOTFS_QCOW2"
    # download source already produces a qcow2; skip the raw→qcow2 conversion below.
    # We still need to customize it via docker overlay, but for the first version
    # we leave it as-is and rely on cloud-init or 9p for tool/env injection.
    ln -sf "$KERNEL_IMAGE" "$OUTPUT_DIR/kernel.img"
    mkdir -p "$OUTPUT_DIR/modules" "$OUTPUT_DIR/tools" "$OUTPUT_DIR/env"
    [[ -d "$MODULES_DIR" ]] && rsync -a "$MODULES_DIR/" "$OUTPUT_DIR/modules/" 2>/dev/null || true
    [[ -d "$TOOLS_DIR" ]] && rsync -a "$TOOLS_DIR/" "$OUTPUT_DIR/tools/" 2>/dev/null || true
    log "download source produced rootfs.qcow2 as-is (no inline customization)"
    log "tools/modules/env are 9p-mounted into guests at runtime"
    echo "$new_sig" >"$sig_file"
    exit 0
    ;;
  dnf)
    log "acquiring rootfs via dnf installroot"
    [[ -d /etc/yum.repos.d ]] || die "dnf source requires a yum-based host (HCE/openEuler/Fedora/RHEL)"
    DNF_ROOT="$TMP_DIR/dnfroot"
    mkdir -p "$DNF_ROOT"
    # Use the host's repos if they are openEuler; otherwise pin an official repo.
    if ! grep -q -i openeuler /etc/os-release 2>/dev/null; then
      cat >"$TMP_DIR/openeuler-local.repo" <<'EOF'
[openeuler-2403]
name=openEuler 24.03 LTS aarch64
baseurl=https://repo.openeuler.org/openEuler-24.03-LTS/OS/aarch64/
enabled=1
gpgcheck=0
EOF
      dnf --installroot="$DNF_ROOT" --releasever=2403 \
          --repo=openeuler-2403 --setopt=reposdir="$TMP_DIR" \
          install -y --nogpgcheck \
          basesystem util-linux procps iproute iputils bash systemd kernel-core
    else
      dnf --installroot="$DNF_ROOT" --releasever=2403 install -y --nogpgcheck \
          basesystem util-linux procps iproute iputils bash systemd kernel-core
    fi
    tar -C "$DNF_ROOT" -cf "$ROOTFS_TAR" .
    ;;
esac

# ---- customize rootfs inside a privileged docker container ----------------
#
# Host has no root, so we can't mount loop locally. We have docker, so we
# run a privileged container that:
#   1. formats the raw image with mkfs.ext4
#   2. tar-unpacks the rootfs into the loop-mounted image
#   3. bakes the -static tools, .ko modules, and our systemd unit into the image
#   4. unmounts and converts the raw to qcow2 (qemu-img installed in-container)
#
# Everything happens inside one container invocation. The customize script
# below is piped in via stdin so we don't have to materialize a separate file
# on the host.

build_raw_rootfs_via_docker() {
  local raw_path="$1"
  local rootfs_tar="$2"
  local tools_dir_host="$3"
  local modules_dir_host="$4"
  local size_gb="$5"
  local qcow2_path="$6"

  local raw_dir raw_name
  raw_dir="$(dirname "$raw_path")"
  raw_name="$(basename "$raw_path")"

  local qcow2_dir qcow2_name
  qcow2_dir="$(dirname "$qcow2_path")"
  qcow2_name="$(basename "$qcow2_path")"

  local tools_dir_abs modules_dir_abs
  tools_dir_abs="$(cd "$tools_dir_host" 2>/dev/null && pwd || echo "$tools_dir_host")"
  modules_dir_abs="$(cd "$modules_dir_host" 2>/dev/null && pwd || echo "$modules_dir_host")"

  local tools_mount_arg="/tools:ro"
  local modules_mount_arg="/modules:ro"
  local tools_volume=()
  local modules_volume=()
  if [[ -d "$tools_dir_abs" ]]; then
    tools_volume=(-v "$tools_dir_abs:/tools:ro")
  fi
  if [[ -d "$modules_dir_abs" ]]; then
    modules_volume=(-v "$modules_dir_abs:/modules:ro")
  fi

  log "running privileged container to build $qcow2_name (tools=$tools_dir_abs modules=$modules_dir_abs)"

  # Materialize the customize script into the same dir we bind-mount as
  # /output, then exec it in-container. This is more debuggable than piping
  # via stdin and survives terminal/stdin quirks.
  local customize_script="$raw_dir/customize_rootfs.sh"
  cat >"$customize_script" <<'CUSTOMIZE'
#!/bin/bash
set -euo pipefail

echo "[customize] installing qemu-img in container"
dnf install -y -q --setopt=install_weak_deps=False qemu-img >/dev/null 2>&1 || \
  echo "[customize] warning: qemu-img install failed, will skip raw->qcow2 conversion"

echo "[customize] formatting /output/$RAW_NAME size=${SIZE_GB}G"
truncate -s "${SIZE_GB}G" "/output/$RAW_NAME"
mkfs.ext4 -F -L openeuler_root "/output/$RAW_NAME" >/dev/null
mkdir -p /mnt
mount -o loop "/output/$RAW_NAME" /mnt
echo "[customize] unpacking rootfs tar"
tar -C /mnt -xf "/output/$ROOTFS_TAR_NAME" 2>/dev/null || true
# docker export may leave whiteout entries that confuse tar; this is fine.

# Bake in -static tools from /tools (host-mounted read-only) into /bin.
# run_app invokes them as /bin/linqu_* (busybox initramfs convention); putting
# them in /bin keeps those paths working without sed-patching run_app. On
# openEuler /bin is a symlink to /usr/bin, so this is /usr/bin/ in practice.
# /bin (the symlink) already exists after tar unpack, so don't try to mkdir.
if [[ -d /tools ]]; then
  echo "[customize] installing -static tools into /bin:"
  for f in /tools/*; do
    [[ -f "$f" ]] || continue
    name="$(basename "$f")"
    cp -a "$f" "/mnt/bin/$name"
    chmod +x "/mnt/bin/$name"
    echo "  $name"
  done
fi

# Bake in .ko modules into /lib/modules/linqu-ub.
if [[ -d /modules ]]; then
  mkdir -p /mnt/lib/modules/linqu-ub
  echo "[customize] installing .ko modules into /lib/modules/linqu-ub:"
  for f in /modules/*.ko; do
    [[ -f "$f" ]] || continue
    cp -a "$f" "/mnt/lib/modules/linqu-ub/$(basename "$f")"
    echo "  $(basename "$f")"
  done
fi

# Serial console policy: NO gettys at all.
#
# The W5 guest service owns /dev/ttyAMA0 (StandardOutput/StandardError=tty,
# TTYPath=/dev/ttyAMA0). Any getty on the same line (serial-getty@ttyAMA0 via
# getty.target.wants, console-getty on /dev/console, or getty@tty1 from the
# base image) races the W5 service for the tty: writes get interleaved/lost,
# and the getty's vhangup when starting agetty killed the running W5 workload
# processes mid-cluster-bootstrap (observed as the deterministic "all 8 nodes
# freeze right after the OBMM datastore imports" stall; root password is
# locked in the base image anyway, so the login prompt was unusable). Mask
# all three getty units at the /etc/systemd/system level — this outranks
# getty.target.wants symlinks and the runtime getty-generator.
mkdir -p /mnt/etc/systemd/system
ln -sf /dev/null /mnt/etc/systemd/system/serial-getty@ttyAMA0.service
ln -sf /dev/null /mnt/etc/systemd/system/console-getty.service
ln -sf /dev/null /mnt/etc/systemd/system/getty@tty1.service
# Belt and braces: drop the base image's own getty.target.wants entries too.
rm -f /mnt/etc/systemd/system/getty.target.wants/serial-getty@ttyAMA0.service
rm -f /mnt/etc/systemd/system/getty.target.wants/getty@tty1.service
rm -f /mnt/etc/systemd/system/getty.target.wants/console-getty.service

# W5 guest unit: mounts 9p tags, sources env, runs run_app.
cat >/mnt/etc/systemd/system/linqu-w5-guest.service <<'UNIT'
[Unit]
Description=W5 guest launch service
After=network.target
After=systemd-modules-load.service
Wants=systemd-modules-load.service

[Service]
Type=oneshot
RemainAfterExit=yes
StandardOutput=tty
StandardError=tty
TTYPath=/dev/ttyAMA0

# Load UB kernel modules. Names must match the .ko basenames (without .ko) in
# /lib/modules/linqu-ub/. The currently shipped set is just linqu_ub_drv. Use
# the - prefix so a missing module doesn't abort the unit; modprobe -d / makes
# it look at the guest's /lib/modules rather than the host's.
ExecStart=-/sbin/modprobe -d / linqu_ub_drv

# 9p mounts: env (per-run), tools (read-only fallback if not baked in), modules (fallback).
ExecStartPre=/bin/mkdir -p /run/w5/env /run/w5/tools /run/w5/modules
ExecStartPre=-/bin/mount -t 9p -o trans=virtio,version=9p2000.L,access=any w5env /run/w5/env
ExecStartPre=-/bin/mount -t 9p -o trans=virtio,version=9p2000.L,access=any,ro w5tools /run/w5/tools
ExecStartPre=-/bin/mount -t 9p -o trans=virtio,version=9p2000.L,access=any,ro w5modules /run/w5/modules

# Per-run env injection: env.sh is written to the 9p mount source
# ($OPENEULER_ENV_DIR/env.sh) by run_llm_infer_eight_node_guest.sh before
# the guest boots. It contains the same exports the busybox runner inlines
# (model/dataset/sampler/shortpath/etc. config). The wrapper script sources
# it then execs run_app, so run_app sees the exports as if they were inline.
ExecStart=/bin/run_app_wrapper

[Install]
WantedBy=multi-user.target
UNIT
ln -sf /etc/systemd/system/linqu-w5-guest.service /mnt/etc/systemd/system/multi-user.target.wants/linqu-w5-guest.service

# run_app_wrapper: tiny shim that sources /run/w5/env/env.sh (the 9p-mounted
# per-run env file) and execs /run/w5/env/runner.sh. The runner is the same
# dispatch logic as the busybox path's per-run initramfs runner (node role
# detection, linqu_llm_infer, serving queue handlers), written fresh per run
# by write_w4_openeuler_env. We don't use /bin/run_app from the repo's
# initramfs/ dir because it dispatches on /proc/cmdline flags (e.g.
# linqu_llm_infer=1) which we don't set on the openEuler kernel cmdline —
# systemd would not tolerate such flags.
cat >/mnt/bin/run_app_wrapper <<'WRAPPER'
#!/bin/sh
set -a
if [ -f /run/w5/env/env.sh ]; then
  . /run/w5/env/env.sh
fi
set +a
if [ -x /run/w5/env/runner.sh ]; then
  exec /run/w5/env/runner.sh "$@"
fi
echo "[run_app_wrapper] FAIL: /run/w5/env/runner.sh missing or not executable" >&2
# Last-resort fallback to the baked-in run_app (will likely print "no app
# flags matched" since the cmdline lacks linqu_llm_infer=1, but it gives us a
# debug shell instead of a silent hang).
exec /bin/run_app "$@"
WRAPPER
chmod +x /mnt/bin/run_app_wrapper

# Bring in the run_app shell script from the repo's initramfs/ dir if present.
# The host bind-mount of the repo to /work is set up by run_w5_in_container.sh;
# here we just look for the script and copy it.
#
# run_app was written for busybox initramfs. For openEuler/systemd we:
#   - swap the shebang from /bin/busybox sh to /bin/sh
#   - replace /bin/busybox <cmd> calls with the standard <cmd> (openEuler has
#     coreutils+util-linux, so mkdir/mount/cat/sed/tail/sleep/rm/sh all exist)
#   - set UB_RUN_APP_FROM_INIT=1 to skip the linqu_init recursion (the busybox
#     init path exec's /bin/linqu_init before reaching run_default_actions;
#     under systemd we go straight to run_default_actions)
#   - install at /bin/run_app so the systemd unit's ExecStart path matches the
#     busybox initramfs convention (and matches run_app's own self-reference
#     when exec'ing /bin/linqu_init, though we skip that recursion)
if [[ -f /work/guest-linux/aarch64/initramfs/run_app ]]; then
  cp /work/guest-linux/aarch64/initramfs/run_app /mnt/bin/run_app
  chmod +x /mnt/bin/run_app
  sed -i \
    -e '1s|#!/bin/busybox sh|#!/bin/sh|' \
    -e 's|/bin/busybox mkdir|mkdir|g' \
    -e 's|/bin/busybox mount|mount|g' \
    -e 's|/bin/busybox cat|cat|g' \
    -e 's|/bin/busybox sed|sed|g' \
    -e 's|/bin/busybox tail|tail|g' \
    -e 's|/bin/busybox sleep|sleep|g' \
    -e 's|/bin/busybox rm|rm|g' \
    -e 's|/bin/busybox sh|/bin/sh|g' \
    /mnt/bin/run_app
  # Insert UB_RUN_APP_FROM_INIT=1 export near the top so run_app skips the
  # linqu_init recursion (we don't ship linqu_init for openEuler).
  sed -i '2i export UB_RUN_APP_FROM_INIT=1' /mnt/bin/run_app
fi

# /init script is NOT needed for openEuler/systemd — systemd is PID 1 and the
# linqu-w5-guest.service unit plays the role of init's run_app dispatch.

# Hostname.
echo "openeuler-w5-guest" > /mnt/etc/hostname

# Disable services we don't need (slower boot, eat memory).
chroot /mnt systemctl disable firewalld.service 2>/dev/null || \
  rm -f /mnt/etc/systemd/system/multi-user.target.wants/firewalld.service
chroot /mnt systemctl disable NetworkManager-wait-online.service 2>/dev/null || \
  rm -f /mnt/etc/systemd/system/multi-user.target.wants/NetworkManager-wait-online.service

# Sync and unmount.
sync
umount /mnt

# Convert raw to qcow2 (in-container qemu-img).
if command -v qemu-img >/dev/null 2>&1; then
  echo "[customize] converting /output/$RAW_NAME to /qcow2/$QCOW2_NAME"
  qemu-img convert -f raw -O qcow2 "/output/$RAW_NAME" "/qcow2/$QCOW2_NAME"
  qemu-img resize "/qcow2/$QCOW2_NAME" "${SIZE_GB}G" >/dev/null
else
  echo "[customize] warning: qemu-img unavailable; copying raw to /qcow2/$QCOW2_NAME (will be raw, not qcow2)"
  cp -a "/output/$RAW_NAME" "/qcow2/$QCOW2_NAME"
fi

echo "[customize] done"
CUSTOMIZE
  chmod +x "$customize_script"

  docker run --rm --privileged --platform linux/arm64 \
    -v "$raw_dir:/output:rw" \
    -v "$qcow2_dir:/qcow2:rw" \
    -v "$REPO_ROOT:/work:ro" \
    "${tools_volume[@]}" \
    "${modules_volume[@]}" \
    -e RAW_NAME="$raw_name" \
    -e ROOTFS_TAR_NAME="rootfs.tar" \
    -e SIZE_GB="$size_gb" \
    -e QCOW2_NAME="$qcow2_name" \
    "$DOCKER_IMAGE" \
    bash /output/customize_rootfs.sh
}

# Helper to fetch the host-side tools dir. The -static tools are normally
# scattered: most live under out/ (linqu_mem_service, linqu_mem_service_qwen3,
# linqu_llm_infer, linqu_w5_serving_control, linqu_pretraining_client) but
# linqu_mem_service_host is built in-tree under the mem_service submodule
# (mem_service/apps/mem_service/linqu_mem_service_host) by
# run_w5_memory_service_bootstrap.sh.
#
# If --tools-dir points at a populated dir, use it as-is. Otherwise collect
# the known tools from out/ and the mem_service submodule into out/guest_tools.
ensure_host_tools_dir() {
  local candidate="$1"
  if [[ -d "$candidate" ]]; then
    return 0
  fi
  candidate="$OUT_ROOT/guest_tools"
  mkdir -p "$candidate"
  # Collect every linqu_* static tool from out/. The busybox initramfs path
  # ships ~20 of these; we mirror that set so the openEuler guest can run the
  # same binaries via /bin/<name> (baked into the rootfs) or /run/w5/tools/
  # (9p fallback). Tools not present in $OUT_ROOT are skipped silently.
  local tool
  for tool in "$OUT_ROOT"/linqu_*; do
    [[ -x "$tool" ]] || continue
    cp -a "$tool" "$candidate/$(basename "$tool")"
  done
  # linqu_mem_service_host lives in the mem_service submodule apps/ dir.
  local mem_service_host_src="$REPO_ROOT/mem_service/apps/mem_service/linqu_mem_service_host"
  if [[ -x "$mem_service_host_src" ]]; then
    cp -a "$mem_service_host_src" "$candidate/linqu_mem_service_host"
  else
    log "warning: linqu_mem_service_host not found at $mem_service_host_src"
    log "  (build it via run_w5_memory_service_bootstrap.sh; guests may still use 9p fallback)"
  fi
  TOOLS_DIR="$candidate"
}

ensure_host_modules_dir() {
  local candidate="$1"
  if [[ -d "$candidate" ]]; then
    return 0
  fi
  candidate="$OUT_ROOT/modules"
  if [[ ! -d "$candidate" ]]; then
    log "warning: no modules dir at $candidate; .ko modules won't be baked into rootfs"
    log "  (guest will try /run/w5/modules/ 9p mount fallback at runtime)"
  fi
  MODULES_DIR="$candidate"
}

ensure_host_tools_dir "$TOOLS_DIR"
ensure_host_modules_dir "$MODULES_DIR"

# ROOTFS_TAR is already at $TMP_DIR/rootfs.tar (set in the docker/download/dnf
# acquisition step). The privileged container bind-mounts $TMP_DIR as
# /output:rw, so it can read the tar directly.
ROOTFS_RAW="$TMP_DIR/rootfs.raw"

# Output dir for qcow2 must already exist (we did mkdir -p $OUTPUT_DIR earlier).
build_raw_rootfs_via_docker "$ROOTFS_RAW" "$ROOTFS_TAR" "$TOOLS_DIR" "$MODULES_DIR" "$ROOTFS_SIZE_GB" "$ROOTFS_QCOW2"

# Build the initramfs containing busybox + virtio modules + init_switch_root.
# Defined earlier (before the signature-skip block) so the skip branch can
# rebuild it on a partial-cache run.
build_openeuler_initramfs

# ---- finalize outputs ------------------------------------------------------

ln -sf "$KERNEL_IMAGE" "$OUTPUT_DIR/kernel.img"
mkdir -p "$OUTPUT_DIR/modules" "$OUTPUT_DIR/tools" "$OUTPUT_DIR/env"
[[ -d "$MODULES_DIR" ]] && rsync -a "$MODULES_DIR/" "$OUTPUT_DIR/modules/" 2>/dev/null || true
[[ -d "$TOOLS_DIR" ]] && rsync -a "$TOOLS_DIR/" "$OUTPUT_DIR/tools/" 2>/dev/null || true

# install the per-node launcher
cat >"$OUTPUT_DIR/run_openeuler_guest.sh" <<LAUNCHER
#!/bin/zsh
# Per-node QEMU launcher for openEuler guest backend.
# Standalone single-node launch — useful for debugging one node at a time.
# For the 8-node cluster, launch_ub_eight_node_headless.sh inlines the same
# argv construction inside start_node() when SIM_W5_GUEST_BACKEND=openeuler,
# so the cluster path doesn't actually exec this script. Keep both in sync.
#
# Args (env vars):
#   NODE_ID       nodeA..nodeH
#   NODE_IP       10.0.0.x
#   KERNEL_IMAGE  path to kernel_ub Image
#   ROOTFS_BASE   path to rootfs.qcow2 (read-only base)
#   OVERLAY_DIR   where to put per-node overlay qcow2
#   ENV_DIR       per-run env dir (9p mount source)
#   TOOLS_DIR     -static tools dir (9p mount source)
#   MODULES_DIR   .ko files dir (9p mount source)
#   QEMU_BIN      qemu-system-aarch64 path
#   QEMU_IMG_BIN  qemu-img path (default: sibling of QEMU_BIN)
#   QEMU_MEM      per-node memory (default 12G)
#   QEMU_SMP      per-node vcpu count (default 4)
#   SERIAL_LOG    per-node serial log path
#   QMP_SOCKET    per-node qmp socket
#   MON_SOCKET    per-node qmp monitor socket (optional)
#   APPEND_EXTRA  kernel cmdline extras
set -euo pipefail

NODE_ID="\${NODE_ID:?missing NODE_ID}"
NODE_IP="\${NODE_IP:?missing NODE_IP}"
KERNEL_IMAGE="\${KERNEL_IMAGE:?missing KERNEL_IMAGE}"
ROOTFS_BASE="\${ROOTFS_BASE:?missing ROOTFS_BASE}"
OVERLAY_DIR="\${OVERLAY_DIR:?missing OVERLAY_DIR}"
ENV_DIR="\${ENV_DIR:?missing ENV_DIR}"
TOOLS_DIR="\${TOOLS_DIR:?missing TOOLS_DIR}"
MODULES_DIR="\${MODULES_DIR:?missing MODULES_DIR}"
QEMU_BIN="\${QEMU_BIN:?missing QEMU_BIN}"
QEMU_MEM="\${QEMU_MEM:-12G}"
QEMU_SMP="\${QEMU_SMP:-4}"
SERIAL_LOG="\${SERIAL_LOG:?missing SERIAL_LOG}"
QMP_SOCKET="\${QMP_SOCKET:?missing QMP_SOCKET}"
APPEND_EXTRA="\${APPEND_EXTRA:-}"

mkdir -p "\$OVERLAY_DIR"
OVERLAY="\$OVERLAY_DIR/\${NODE_ID}.qcow2"
if [[ ! -f "\$OVERLAY" ]]; then
  QEMU_IMG_BIN="\${QEMU_IMG_BIN:-}"
  [[ -z "\$QEMU_IMG_BIN" ]] && QEMU_IMG_BIN="\$(dirname "\$QEMU_BIN")/qemu-img"
  if [[ ! -x "\$QEMU_IMG_BIN" ]]; then
    QEMU_IMG_BIN="\$(command -v qemu-img 2>/dev/null || true)"
  fi
  if [[ -n "\$QEMU_IMG_BIN" ]]; then
    "\$QEMU_IMG_BIN" create -f qcow2 -F raw -b "\$ROOTFS_BASE" "\$OVERLAY" >/dev/null
  else
    echo "[run_openeuler_guest] qemu-img not found; using base image directly" >&2
    OVERLAY="\$ROOTFS_BASE"
  fi
fi

INITRAMFS_IMAGE="\${INITRAMFS_IMAGE:-\$(dirname "\$ROOTFS_BASE")/initramfs.cpio.gz}"

guest_qemu_args=(
  -S
  -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on
  -cpu cortex-a57
  -smp "\$QEMU_SMP"
  -m "\$QEMU_MEM"
  -nodefaults
  -display none
  -qmp unix:"\$QMP_SOCKET",server=on,wait=off
  -chardev socket,id=ser0,path="\${SERIAL_LOG%.log}.sock",server=on,wait=off,logfile="\$SERIAL_LOG",logappend=on
  -serial chardev:ser0
  -drive "file=\$OVERLAY,format=qcow2,if=virtio"
  -virtfs "local,path=\$ENV_DIR,mount_tag=w5env,security_model=none,fmode=0644,dmode=0755"
  -virtfs "local,path=\$TOOLS_DIR,mount_tag=w5tools,security_model=none,fmode=0755,dmode=0755"
  -virtfs "local,path=\$MODULES_DIR,mount_tag=w5modules,security_model=none,fmode=0644,dmode=0755"
  -kernel "\$KERNEL_IMAGE"
  -initrd "\$INITRAMFS_IMAGE"
  -append "console=ttyAMA0 root=/dev/vda rw rootfstype=ext4 init=/init linqu_ipourma_ipv4=\$NODE_IP \${APPEND_EXTRA}"
)

exec "\$QEMU_BIN" "\${guest_qemu_args[@]}" >"\${SERIAL_LOG%.log}.qemu.log" 2>&1 &
LAUNCHER
chmod +x "$OUTPUT_DIR/run_openeuler_guest.sh"

echo "$new_sig" >"$sig_file"
log "done. outputs in $OUTPUT_DIR:"
log "  rootfs.qcow2 ($(du -h "$ROOTFS_QCOW2" | cut -f1))"
log "  initramfs.cpio.gz ($(du -h "$OUTPUT_DIR/initramfs.cpio.gz" | cut -f1))"
log "  kernel.img -> $KERNEL_IMAGE"
log "  modules/ tools/ env/ (9p mount sources)"
log "  run_openeuler_guest.sh (per-node launcher)"
log ""
log "to use: SIM_W5_GUEST_BACKEND=openeuler ./scripts/run_w5_in_container.sh -- w5.env"
