#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENGINE="${SIM_GUEST_ENGINE:-initramfs}"
DISK_IMAGE="${SIM_GUEST_DISK_IMAGE:-}"

usage() {
  cat <<'USAGE'
usage: run_ub_demo.sh [--guest-engine initramfs|openEuler]
                      [--open-euler-disk-image PATH] -- ENTRYPOINT [ARGS...]

ENTRYPOINT is a supported Console demo script in guest-linux/aarch64/scripts.
openEuler boots a fresh per-node disk overlay and deploys current guest apps.
Runtime options may also be loaded from a .env file before this command.
USAGE
}

while (( $# )); do
  case "$1" in
    --guest-engine|--open-euler-disk-image)
      (( $# >= 2 )) || { echo "$1 requires a value" >&2; exit 2; }
      if [[ "$1" == "--guest-engine" ]]; then ENGINE="$2"; else DISK_IMAGE="$2"; fi
      shift 2
      ;;
    --) shift; break ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
case "$ENGINE" in
  initramfs|busybox) ENGINE=initramfs; DISK_IMAGE="" ;;
  openEuler|open_euler|openeuler) ENGINE=openEuler ;;
  *) echo "unknown guest engine: $ENGINE" >&2; exit 2 ;;
esac
(( $# )) || { usage >&2; exit 2; }
ENTRYPOINT="$1"
shift
case "$ENTRYPOINT" in
  run_ub_dual_node_apps.sh|run_ub_dual_node_rpc.sh|run_ub_dual_node_mem_service.sh|\
  run_ub_dual_node_obmm_pool.sh|run_ub_dual_node_obmm_gsva.sh|\
  run_ub_four_node_obmm_pool.sh|run_ub_eight_node_obmm_pool.sh|\
  run_ub_four_node_rpc_matrix.sh|run_ub_eight_node_rpc_matrix.sh|\
  run_ub_two_node_gsva_identity_test.sh|run_ub_four_node_gsva_identity_test.sh|\
  run_ub_eight_node_gsva_identity_test.sh|run_ub_multi_node_obmm_gsva_matrix.sh|\
  launch_ub_four_node_headless.sh|launch_ub_eight_node_headless.sh|\
  run_w5_cluster_config.sh) ;;
  *) echo "unsupported demo entrypoint: $ENTRYPOINT" >&2; exit 2 ;;
esac
[[ "$ENTRYPOINT" != */* && -f "$SCRIPT_DIR/$ENTRYPOINT" ]] || {
  echo "demo entrypoint not found: $ENTRYPOINT" >&2; exit 2;
}
if [[ "$ENGINE" == "openEuler" ]]; then
  [[ -f "$DISK_IMAGE" ]] || { echo "openEuler disk image not found: $DISK_IMAGE" >&2; exit 2; }
  DISK_IMAGE="$(cd "$(dirname "$DISK_IMAGE")" && pwd)/$(basename "$DISK_IMAGE")"
fi
export SIM_GUEST_ENGINE="$ENGINE" SIM_GUEST_DISK_IMAGE="$DISK_IMAGE"
export SIM_W5_GUEST_BACKEND=busybox
if [[ "$ENGINE" == "initramfs" ]]; then
  unset SIM_W5_OE_DISK_IMAGE
fi
if [[ "$ENTRYPOINT" == "run_w5_cluster_config.sh" ]]; then
  if [[ "$ENGINE" == "openEuler" ]]; then
    exec "$SCRIPT_DIR/$ENTRYPOINT" --open-euler-disk-image "$DISK_IMAGE" "$@"
  fi
fi
exec "$SCRIPT_DIR/$ENTRYPOINT" "$@"
