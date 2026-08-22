#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# W5 qwen3-0.6B decode cluster on openEuler guests.
# Same W5 cluster config flow as run_w5_cluster_qwen3_0_6b_2step.sh, but each
# node boots openEuler from a per-run qcow2 overlay of SIM_W5_OE_DISK_IMAGE
# and the W5 run_app flow is started by a generated systemd unit instead of
# the busybox initramfs rdinit chain.
#
# Requirements:
#   - SIM_W5_OE_DISK_IMAGE (or --disk): openEuler qcow2 image whose root LV
#     is openeuler_bogon-root (the stock openEuler-2403 layout works).
#   - The one-time LVM2 staging extraction needs sudo on the host.

DISK_IMAGE="${SIM_W5_OE_DISK_IMAGE:-${1:-}}"
if [[ -z "$DISK_IMAGE" && -f /sd_data/vms/openEuler-2403/disk.qcow2 ]]; then
  DISK_IMAGE=/sd_data/vms/openEuler-2403/disk.qcow2
fi
if [[ -z "$DISK_IMAGE" ]]; then
  echo "usage: $0 [--disk PATH]  (or set SIM_W5_OE_DISK_IMAGE)" >&2
  exit 2
fi
shift $(( $# > 0 ? 1 : 0 ))

export SIM_W5_GUEST_ENGINE=openEuler
export SIM_W5_OE_DISK_IMAGE="$DISK_IMAGE"
export QEMU_MEM="${QEMU_MEM:-8G}"
export SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-4}"

exec "$SCRIPT_DIR/run_w5_cluster_qwen3_0_6b_2step.sh" "$@"
