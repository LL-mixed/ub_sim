#!/bin/zsh
set -euo pipefail
setopt null_glob

# ------------------------------------------------------------------
# run-openEuler-simulated-super-node.sh
# Launch a multi-node UB simulation where each node boots openEuler
# from a qcow2 disk image.
# ------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"

# ------------------------------------------------------------------
# Defaults
# ------------------------------------------------------------------
DISK_IMAGE=""
NODE_COUNT=2
TOPOLOGY_FILE="$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini"
ENTITY_PLAN_FILE="$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini"
ENTITY_COUNT=2
PORT_NUM=1
MEMORY="4G"
SMP=4
APP_DIR=""
DEMO_MODE=""
OUT_DIR="$ROOT_DIR/out/openEuler-super-node"
RUN_ID=""
KERNEL_IMAGE="$ROOT_DIR/out/Image"
QEMU_MEM="${QEMU_MEM:-$MEMORY}"
QEMU_SMP="${QEMU_SMP:-$SMP}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

# LVM2 tools staging (extracted from openEuler disk once)
LVM2_STAGING_DIR="${LVM2_STAGING_DIR:-/tmp/oe_lvm2_tools}"

source "$SCRIPT_DIR/qemu_ub_common.sh"

# ------------------------------------------------------------------
# Usage
# ------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") --disk PATH [OPTIONS]

Launch a simulated super-node where each QEMU node boots openEuler.

Required:
  --disk PATH          Path to openEuler qcow2 disk image

Options:
  --nodes N            Number of nodes: 2, 4, or 8 (default: 2)
  --topology FILE      Topology ini file (default: vendor/ub_topology_two_node_v0.ini)
  --memory SIZE        QEMU memory per node, e.g. 4G (default: 4G)
  --smp N              QEMU SMP per node (default: 4)
  --app-dir DIR        Directory with apps/demos to deploy into each node
  --demo MODE          Demo mode hint (e.g. gva_direct, gsva_matrix, obmm_coh)
  --out-dir DIR        Output directory for node artifacts (default: out/openEuler-super-node)
  --run-id ID          Custom run-id prefix
  --append-extra STR   Extra kernel append string
  -h, --help           Show this help

Examples:
  $(basename "$0") --disk ~/vms/openEuler-2403/disk.qcow2 --nodes 8 --memory 8G --smp 4
  $(basename "$0") --disk ~/vms/openEuler-2403/disk.qcow2 --nodes 4 --app-dir ./my_apps --demo gsva_identity
EOF
}

# ------------------------------------------------------------------
# Argument parsing
# ------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --disk)
            DISK_IMAGE="$2"
            shift 2
            ;;
        --nodes)
            NODE_COUNT="$2"
            shift 2
            ;;
        --topology)
            TOPOLOGY_FILE="$2"
            shift 2
            ;;
        --memory)
            QEMU_MEM="$2"
            shift 2
            ;;
        --smp)
            QEMU_SMP="$2"
            shift 2
            ;;
        --app-dir)
            APP_DIR="$2"
            shift 2
            ;;
        --demo)
            DEMO_MODE="$2"
            shift 2
            ;;
        --out-dir)
            OUT_DIR="$2"
            shift 2
            ;;
        --run-id)
            RUN_ID="$2"
            shift 2
            ;;
        --append-extra)
            APPEND_EXTRA="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ -z "$DISK_IMAGE" ]]; then
    echo "ERROR: --disk is required" >&2
    usage >&2
    exit 1
fi

if [[ ! -f "$DISK_IMAGE" ]]; then
    echo "ERROR: disk image not found: $DISK_IMAGE" >&2
    exit 1
fi

case "$NODE_COUNT" in
    2)
        NODE_NAMES=(nodeA nodeB)
        NODE_IPS=(10.0.0.1 10.0.0.2)
        DEFAULT_TOPOLOGY="$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini"
        ;;
    4)
        NODE_NAMES=(nodeA nodeB nodeC nodeD)
        NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)
        DEFAULT_TOPOLOGY="$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh.ini"
        ;;
    8)
        NODE_NAMES=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
        NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
        DEFAULT_TOPOLOGY="$WORKSPACE_ROOT/vendor/ub_topology_eight_node_full_mesh.ini"
        ;;
    *)
        echo "ERROR: unsupported node count: $NODE_COUNT (must be 2, 4, or 8)" >&2
        exit 1
        ;;
esac

if [[ "$TOPOLOGY_FILE" == "$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini" && "$NODE_COUNT" != 2 ]]; then
    TOPOLOGY_FILE="$DEFAULT_TOPOLOGY"
fi

RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_oe${NODE_COUNT}_${RANDOM}}"
LOG_PREFIX="[oe-super${NODE_COUNT}]"
LOG_DIR="$OUT_DIR/logs/$RUN_ID"
NODE_ARTIFACTS_DIR="$OUT_DIR/nodes/$RUN_ID"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-oe-${RUN_ID}}"
QMP_DIR="$SHARED_DIR/qmp"
SERIAL_DIR="$SHARED_DIR/serial"
MON_DIR="$SHARED_DIR/mon"
UB_QEMU_RUNTIME_DIR="${UB_QEMU_RUNTIME_DIR:-$SHARED_DIR/xdg_runtime}"

# ------------------------------------------------------------------
# Ensure QEMU binary
# ------------------------------------------------------------------
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"

# ------------------------------------------------------------------
# Ensure kernel image exists
# ------------------------------------------------------------------
if [[ ! -f "$KERNEL_IMAGE" ]]; then
    echo "$LOG_PREFIX kernel image not found: $KERNEL_IMAGE" >&2
    echo "$LOG_PREFIX hint: build kernel first with build_guest_artifacts.sh" >&2
    exit 1
fi

# ------------------------------------------------------------------
# Logging helpers
# ------------------------------------------------------------------
log() {
    echo "$LOG_PREFIX $*" | tee -a "$LOG_DIR/run.log" 2>/dev/null || echo "$LOG_PREFIX $*"
}

# ------------------------------------------------------------------
# Extract LVM2 tools from openEuler disk (one-time)
# ------------------------------------------------------------------
ensure_lvm2_staging() {
    # Recreate staging if it exists but is not usable (e.g. root-owned or incomplete)
    if [[ -d "$LVM2_STAGING_DIR" ]]; then
        if [[ -r "$LVM2_STAGING_DIR/lvm" && -r "$LVM2_STAGING_DIR/vgscan" ]]; then
            return 0
        fi
        log "  existing LVM2 staging is unusable, recreating..."
        rm -rf "$LVM2_STAGING_DIR"
    fi

    log "extracting LVM2 tools from openEuler disk..."
    mkdir -p "$LVM2_STAGING_DIR"

    # Convert qcow2 to raw temporarily for mounting
    local raw_img="/tmp/oe_disk_$$.raw"
    qemu-img convert -f qcow2 -O raw -S 512 "$DISK_IMAGE" "$raw_img"

    local loop_dev=""
    loop_dev="$(sudo losetup -f --show "$raw_img")"
    sudo partprobe "$loop_dev"
    sudo pvscan >/dev/null 2>&1
    sudo vgscan >/dev/null 2>&1
    sudo vgchange -ay openeuler_bogon >/dev/null 2>&1

    sudo mkdir -p /mnt/oe_staging_$$
    sudo mount /dev/mapper/openeuler_bogon-root /mnt/oe_staging_$$

    # Copy binaries
    for bin in lvm vgscan vgchange pvscan dmsetup; do
        if [[ -f "/mnt/oe_staging_$$/usr/sbin/$bin" ]]; then
            sudo cp -L "/mnt/oe_staging_$$/usr/sbin/$bin" "$LVM2_STAGING_DIR/"
        fi
    done

    # Copy config (only the files needed; skip root-owned archive/backup/cache dirs)
    sudo mkdir -p "$LVM2_STAGING_DIR/etc/lvm"
    for cfg in lvm.conf lvmlocal.conf; do
        if [[ -f "/mnt/oe_staging_$$/etc/lvm/$cfg" ]]; then
            sudo cp -L "/mnt/oe_staging_$$/etc/lvm/$cfg" "$LVM2_STAGING_DIR/etc/lvm/"
        fi
    done

    # Copy libraries (use chroot-relative paths for ldd)
    for bin_path in /usr/sbin/lvm /usr/sbin/dmsetup; do
        if [[ ! -f "/mnt/oe_staging_$$$bin_path" ]]; then
            continue
        fi
        sudo chroot /mnt/oe_staging_$$ /usr/bin/ldd "$bin_path" 2>/dev/null | grep '=> /' | awk '{print $3}' | while read lib; do
            local src="/mnt/oe_staging_$$$lib"
            if [[ -f "$src" ]]; then
                sudo cp -L "$src" "$LVM2_STAGING_DIR/"
            fi
        done
    done

    # Copy transitive library dependencies
    for lib in "$LVM2_STAGING_DIR"/*.so*; do
        [[ -f "$lib" ]] || continue
        local lib_name
        lib_name="$(basename "$lib")"
        sudo chroot /mnt/oe_staging_$$ /usr/bin/ldd "$lib_name" 2>/dev/null | grep '=> /' | awk '{print $3}' | while read dep; do
            local dep_src="/mnt/oe_staging_$$$dep"
            local dep_dst="$LVM2_STAGING_DIR/$(basename "$dep")"
            if [[ -f "$dep_src" && ! -f "$dep_dst" ]]; then
                sudo cp -L "$dep_src" "$dep_dst"
            fi
        done
    done

    # Copy linker
    if [[ -f /mnt/oe_staging_$$/lib/ld-linux-aarch64.so.1 ]]; then
        sudo cp -L /mnt/oe_staging_$$/lib/ld-linux-aarch64.so.1 "$LVM2_STAGING_DIR/"
    elif [[ -f /mnt/oe_staging_$$/lib64/ld-linux-aarch64.so.1 ]]; then
        sudo cp -L /mnt/oe_staging_$$/lib64/ld-linux-aarch64.so.1 "$LVM2_STAGING_DIR/"
    fi

    # Fix permissions
    sudo chown -R "$(id -u):$(id -g)" "$LVM2_STAGING_DIR"
    chmod +x "$LVM2_STAGING_DIR"/* 2>/dev/null || true

    # Cleanup
    sudo umount /mnt/oe_staging_$$ 2>/dev/null || true
    sudo rmdir /mnt/oe_staging_$$ 2>/dev/null || true
    sudo vgchange -an openeuler_bogon >/dev/null 2>&1 || true
    sudo losetup -d "$loop_dev" 2>/dev/null || true
    rm -f "$raw_img"

    log "  LVM2 staging ready at $LVM2_STAGING_DIR"
}

# ------------------------------------------------------------------
# Build per-node initramfs
# ------------------------------------------------------------------
build_node_initramfs() {
    local node_idx="$1"
    local node_name="$2"
    local out_initramfs="$3"

    local tmpdir="/tmp/oe_initramfs_${node_name}_$$"
    rm -rf "$tmpdir"
    mkdir -p "$tmpdir"

    # 1. busybox
    mkdir -p "$tmpdir/bin"
    cp -L "$BUSYBOX" "$tmpdir/bin/busybox" 2>/dev/null || cp "$BUSYBOX" "$tmpdir/bin/busybox"
    chmod +x "$tmpdir/bin/busybox"
    for cmd in sh echo cat mount umount mkdir sleep ls cp mv rm basename readlink insmod modprobe switch_root; do
        ln -sf busybox "$tmpdir/bin/$cmd" 2>/dev/null || true
    done

    # 2. init script
    cp "$ROOT_DIR/initramfs/init_switch_root" "$tmpdir/init"
    chmod +x "$tmpdir/init"

    # 3. LVM2 tools + libraries
    mkdir -p "$tmpdir/sbin" "$tmpdir/lib"
    for bin in lvm vgscan vgchange pvscan dmsetup; do
        if [[ -f "$LVM2_STAGING_DIR/$bin" ]]; then
            cp -L "$LVM2_STAGING_DIR/$bin" "$tmpdir/sbin/"
        fi
    done
    for lib in "$LVM2_STAGING_DIR"/*.so* "$LVM2_STAGING_DIR"/ld-linux*; do
        [[ -f "$lib" ]] || continue
        cp -L "$lib" "$tmpdir/lib/"
    done
    if [[ -d "$LVM2_STAGING_DIR/etc/lvm" ]]; then
        mkdir -p "$tmpdir/etc"
        cp -r "$LVM2_STAGING_DIR/etc/lvm" "$tmpdir/etc/"
    fi

    # 4. UB kernel modules (those still built as =m)
    mkdir -p "$tmpdir/lib/modules"
    local mod_src="$ROOT_DIR/out/kernel_build"
    for ko in \
        "drivers/ub/ubase/ubase.ko" \
        "drivers/ub/urma/ubcore/ubcore.ko" \
        "drivers/ub/urma/hw/udma/udma.ko" \
        "drivers/ub/urma/ulp/ipourma/ipourma.ko" \
        "drivers/ub/urma/uburma/uburma.ko" \
        "drivers/iommu/hisilicon/ummu-core/ummu-core.ko" \
        "drivers/iommu/hisilicon/ummu.ko"; do
        if [[ -f "$mod_src/$ko" ]]; then
            cp "$mod_src/$ko" "$tmpdir/lib/modules/"
        fi
    done

    # 5. Apps to deploy
    if [[ -n "$APP_DIR" && -d "$APP_DIR" ]]; then
        mkdir -p "$tmpdir/ub_apps"
        cp -r "$APP_DIR"/* "$tmpdir/ub_apps/" 2>/dev/null || true
    fi

    # 6. Package initramfs
    (
        cd "$tmpdir"
        find . | cpio -o -H newc 2>/dev/null | gzip > "$out_initramfs"
    )

    rm -rf "$tmpdir"
}

# ------------------------------------------------------------------
# Create qcow2 overlay for a node
# ------------------------------------------------------------------
create_node_overlay() {
    local base_disk="$1"
    local overlay_path="$2"

    if [[ -f "$overlay_path" ]]; then
        log "  reusing existing overlay: $overlay_path"
        return 0
    fi

    mkdir -p "$(dirname "$overlay_path")"
    qemu-img create -f qcow2 -b "$base_disk" -F qcow2 "$overlay_path"
    log "  created overlay: $overlay_path"
}

# ------------------------------------------------------------------
# Launch a single node
# ------------------------------------------------------------------
launch_node() {
    local idx="$1"
    local node_name="$2"
    local local_ip="$3"
    local overlay_path="$4"
    local initramfs_path="$5"
    local qemu_log="$6"
    local guest_log="$7"
    local pid_file="$8"
    local qmp_socket="$9"
    local mon_socket="${10}"
    local serial_socket="${11}"

    local node_append_extra="$APPEND_EXTRA linqu_ipourma_ipv4=$local_ip"

    log "launching $node_name ip=$local_ip"

    # Build QEMU command
    env \
        UB_FM_NODE_ID="$node_name" \
        UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
        UB_FM_SHARED_DIR="$SHARED_DIR" \
        UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
        UB_SIM_PORT_NUM="$PORT_NUM" \
        XDG_RUNTIME_DIR="$UB_QEMU_RUNTIME_DIR" \
        "$QEMU_BIN" \
            -S \
            -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
            -cpu cortex-a57 \
            -smp "$QEMU_SMP" \
            -m "$QEMU_MEM" \
            -nodefaults \
            -display none \
            -qmp unix:"$qmp_socket",server=on,wait=off \
            -chardev socket,id=mon0,path="$mon_socket",server=on,wait=off \
            -mon chardev=mon0,mode=readline \
            -chardev socket,id=ser0,path="$serial_socket",server=on,wait=off,logfile="$guest_log",logappend=on \
            -serial chardev:ser0 \
            -kernel "$KERNEL_IMAGE" \
            -initrd "$initramfs_path" \
            -drive file="$overlay_path",format=qcow2,if=virtio \
            -append "console=ttyAMA0 root=/dev/mapper/openeuler_bogon-root rw init=/init ${node_append_extra}" \
            >"$qemu_log" 2>&1 &

    echo $! > "$pid_file"
}

# ------------------------------------------------------------------
# Wait for QMP socket
# ------------------------------------------------------------------
wait_for_qmp() {
    local node_name="$1"
    local qmp_socket="$2"
    local pid_file="$3"
    local max_wait=30

    local attempt=0
    while (( attempt < max_wait * 10 )); do
        if [[ -S "$qmp_socket" ]]; then
            return 0
        fi
        if [[ -f "$pid_file" ]]; then
            local pid
            pid="$(cat "$pid_file" 2>/dev/null || true)"
            if [[ -n "${pid:-}" ]] && ! kill -0 "$pid" 2>/dev/null; then
                log "ERROR: qemu exited before QMP ready: $node_name"
                return 1
            fi
        fi
        sleep 0.1
        attempt=$((attempt + 1))
    done

    log "ERROR: QMP socket timeout: $node_name"
    return 1
}

# ------------------------------------------------------------------
# Continue QEMU via QMP
# ------------------------------------------------------------------
cont_qemu() {
    local qmp_socket="$1"
    # Use -q 0 so nc exits after stdin EOF; timeout guards against a stuck monitor.
    {
        sleep 0.1
        echo '{"execute": "qmp_capabilities"}'
        sleep 0.1
        echo '{"execute": "cont"}'
        sleep 0.1
    } | timeout 5 nc -U -q 0 "$qmp_socket" >/dev/null 2>&1 || true
}

# ------------------------------------------------------------------
# Main
# ------------------------------------------------------------------
mkdir -p "$LOG_DIR" "$NODE_ARTIFACTS_DIR" "$QMP_DIR" "$SERIAL_DIR" "$MON_DIR"
touch "$LOG_DIR/run.log"

log "run_id=$RUN_ID"
log "disk=$DISK_IMAGE"
log "nodes=$NODE_COUNT"
log "topology=$TOPOLOGY_FILE"
log "memory=$QEMU_MEM smp=$QEMU_SMP"
log "out_dir=$OUT_DIR"
if [[ -n "$APP_DIR" ]]; then
    log "app_dir=$APP_DIR"
fi
if [[ -n "$DEMO_MODE" ]]; then
    log "demo_mode=$DEMO_MODE"
fi

# Extract LVM2 tools (one-time)
ensure_lvm2_staging

# Generate cleanup script
CLEANUP_SCRIPT="$NODE_ARTIFACTS_DIR/cleanup.sh"
cat > "$CLEANUP_SCRIPT" <<'EOC'
#!/bin/zsh
set -euo pipefail
for pid_file in "__NODE_ARTIFACTS_DIR__"/*.pid; do
    [[ -f "$pid_file" ]] || continue
    pid="$(cat "$pid_file" 2>/dev/null || true)"
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        sleep 0.2
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pid_file"
done
rm -rf "__SHARED_DIR__"
echo "cleaned run_id=__RUN_ID__"
EOC
perl -0pi -e 's#__NODE_ARTIFACTS_DIR__#'"$NODE_ARTIFACTS_DIR"'#g; s#__SHARED_DIR__#'"$SHARED_DIR"'#g; s#__RUN_ID__#'"$RUN_ID"'#g' "$CLEANUP_SCRIPT"
chmod +x "$CLEANUP_SCRIPT"

log "cleanup_script=$CLEANUP_SCRIPT"

# Setup per-node artifacts and launch
integer idx=0
for node_name in "${NODE_NAMES[@]}"; do
    local_ip="${NODE_IPS[$((idx+1))]}"

    overlay_path="$NODE_ARTIFACTS_DIR/${node_name}_disk.qcow2"
    initramfs_path="$NODE_ARTIFACTS_DIR/${node_name}_initramfs.cpio.gz"
    qemu_log="$LOG_DIR/${node_name}_qemu.log"
    guest_log="$LOG_DIR/${node_name}_guest.log"
    pid_file="$NODE_ARTIFACTS_DIR/${node_name}.pid"
    qmp_socket="$QMP_DIR/${node_name}.sock"
    mon_socket="$MON_DIR/${node_name}.sock"
    serial_socket="$SERIAL_DIR/${node_name}.sock"

    log "preparing $node_name..."

    # Create overlay
    create_node_overlay "$DISK_IMAGE" "$overlay_path"

    # Build initramfs
    build_node_initramfs "$idx" "$node_name" "$initramfs_path"

    # Launch
    launch_node "$idx" "$node_name" "$local_ip" "$overlay_path" "$initramfs_path" \
        "$qemu_log" "$guest_log" "$pid_file" "$qmp_socket" "$mon_socket" "$serial_socket"

    idx=$((idx + 1))
    sleep 0.2
done

# Wait for QMP sockets and resume
log "waiting for QMP sockets..."
for node_name in "${NODE_NAMES[@]}"; do
    qmp_socket="$QMP_DIR/${node_name}.sock"
    pid_file="$NODE_ARTIFACTS_DIR/${node_name}.pid"
    if ! wait_for_qmp "$node_name" "$qmp_socket" "$pid_file"; then
        log "failed to get QMP for $node_name, aborting"
        bash "$CLEANUP_SCRIPT" >/dev/null 2>&1 || true
        exit 1
    fi
    cont_qemu "$qmp_socket"
    log "resumed $node_name"
done

# Summary
cat <<EOF

${LOG_PREFIX} All $NODE_COUNT nodes launched.
${LOG_PREFIX} Run ID: $RUN_ID
${LOG_PREFIX} Logs:   $LOG_DIR
${LOG_PREFIX} Nodes:  $NODE_ARTIFACTS_DIR
${LOG_PREFIX} Cleanup: $CLEANUP_SCRIPT

# To attach to a node console:
  sudo socat - UNIX-CONNECT:$SERIAL_DIR/nodeA.sock

# To monitor via QMP:
  echo '{"execute":"query-status"}' | sudo nc -U $QMP_DIR/nodeA.sock

# To clean up all nodes:
  $CLEANUP_SCRIPT

EOF

# If demo mode was specified, optionally wait and report
if [[ -n "$DEMO_MODE" ]]; then
    log "demo_mode=$DEMO_MODE: nodes are running, use cleanup script when done"
fi
