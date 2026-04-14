#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../../.." && pwd)"
QEMU_DIR="${QEMU_DIR:-}"
SCENARIO="$WORKSPACE_ROOT/simulator/scenarios/mvp_2host_single_domain.yaml"
DEFAULT_KERNEL_IMAGE="$WORKSPACE_ROOT/simulator/archive/guest-probe-legacy/aarch64/linux_blobs/Image"
DEFAULT_INITRAMFS_IMAGE="$WORKSPACE_ROOT/simulator/archive/guest-probe-legacy/aarch64/linux_blobs/initramfs.cpio.gz"
OUT_DIR="$ROOT_DIR/out"
PID_FILE="$OUT_DIR/linux_probe.qemu.pid"
SERIAL_LOG="$OUT_DIR/linux_probe.serial.log"
RUN_SECS="${RUN_SECS:-8}"
MACHINE="${MACHINE:-virt}"
MACHINE_OPTS="${MACHINE_OPTS:-}"
QEMU_DEBUG_FLAGS="${QEMU_DEBUG_FLAGS:-}"
QEMU_DEBUG_LOG="${QEMU_DEBUG_LOG:-$OUT_DIR/linux_probe.qemu.log}"
EXTRA_QEMU_ARGS="${EXTRA_QEMU_ARGS:-}"
APPEND_EXTRA="${APPEND_EXTRA:-}"

usage() {
  cat <<'EOF'
Usage: run_linux_probe.sh --legacy [--help]

This is a legacy linqu-ub probe script and is intentionally guarded.
Set QEMU_DIR explicitly, for example:
  QEMU_DIR=/path/to/legacy/qemu ./run_linux_probe.sh --legacy
EOF
}

LEGACY_MODE=0
for arg in "$@"; do
  case "$arg" in
    --legacy)
      LEGACY_MODE=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$LEGACY_MODE" -ne 1 ]]; then
  echo "Refusing to run legacy linqu-ub probe without --legacy." >&2
  usage >&2
  exit 2
fi

: "${KERNEL_IMAGE:=$DEFAULT_KERNEL_IMAGE}"
: "${INITRAMFS_IMAGE:=$DEFAULT_INITRAMFS_IMAGE}"

if [[ -z "$QEMU_DIR" ]]; then
  echo "QEMU_DIR is required for legacy linqu-ub probe script." >&2
  echo "Active dual-node flow uses simulator/vendor/qemu_8.2.0_ub scripts." >&2
  exit 2
fi

if [[ ! -f "$KERNEL_IMAGE" ]]; then
  echo "KERNEL_IMAGE not found: $KERNEL_IMAGE" >&2
  exit 1
fi

if [[ ! -f "$INITRAMFS_IMAGE" ]]; then
  bash "$SCRIPT_DIR/build_initramfs.sh" >/dev/null
  INITRAMFS_IMAGE="$OUT_DIR/initramfs.cpio.gz"
fi

cleanup_previous() {
  if [[ -f "$PID_FILE" ]]; then
    local old_pid
    old_pid="$(cat "$PID_FILE" 2>/dev/null || true)"
    if [[ -n "${old_pid:-}" ]] && kill -0 "$old_pid" 2>/dev/null; then
      kill "$old_pid" 2>/dev/null || true
      sleep 0.2
      kill -9 "$old_pid" 2>/dev/null || true
    fi
    rm -f "$PID_FILE"
  fi
}

cleanup_current() {
  local pid=""
  if [[ -f "$PID_FILE" ]]; then
    pid="$(cat "$PID_FILE" 2>/dev/null || true)"
  fi
  if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
    kill "$pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
}

cleanup_previous
trap cleanup_current EXIT INT TERM

rm -f "$SERIAL_LOG" "$QEMU_DEBUG_LOG"

debug_args=()
if [[ -n "$QEMU_DEBUG_FLAGS" ]]; then
  debug_args=(-d ${=QEMU_DEBUG_FLAGS} -D "$QEMU_DEBUG_LOG")
fi

extra_args=()
if [[ -n "$EXTRA_QEMU_ARGS" ]]; then
  extra_args=(${=EXTRA_QEMU_ARGS})
fi

machine_args=("-M" "$MACHINE")
if [[ -n "$MACHINE_OPTS" ]]; then
  machine_args=("-M" "$MACHINE,$MACHINE_OPTS")
fi

cd "$QEMU_DIR"
./build/qemu-system-aarch64 \
  "${machine_args[@]}" \
  -cpu cortex-a57 \
  -m 512M \
  -nodefaults \
  -nographic \
  -monitor none \
  -serial stdio \
  "${debug_args[@]}" \
  "${extra_args[@]}" \
  -device "linqu-ub,scenario-path=$SCENARIO" \
  -kernel "$KERNEL_IMAGE" \
  -initrd "$INITRAMFS_IMAGE" \
  -append "console=ttyAMA0 rdinit=/init linqu_init_action=probe ${APPEND_EXTRA}" \
  >"$SERIAL_LOG" 2>&1 &

echo $! > "$PID_FILE"
sleep "$RUN_SECS"
cleanup_current
cat "$SERIAL_LOG"
