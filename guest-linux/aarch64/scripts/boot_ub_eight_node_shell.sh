#!/bin/zsh
set -euo pipefail

# Boot an 8-node UB-interconnected openEuler cluster for INTERACTIVE use.
#
# Unlike run_w5_in_container.sh (which drives the W5 inference workload and
# tears the cluster down), this script boots the 8 QEMU nodes and leaves them
# running with a root shell on each node's serial console. Attach from the
# host to any node's console socket and execute commands inside the guest.
#
# Mechanism: the openEuler rootfs runs linqu-w5-guest.service at boot, which
# 9p-mounts $OPENEULER_GUEST_DIR/env at /run/w5/env, sources env.sh and execs
# runner.sh as root with the serial console (ttyAMA0) attached. We write a
# runner.sh that starts an interactive bash on ttyAMA0 instead of the W5
# workload. Note: a later W5 run rewrites env.sh/runner.sh — re-run this
# script (up) to restore shell mode.
#
# QEMU must run inside the repo's build container (the prebuilt binary links
# liburing/pixman from the image, not the host), so the cluster runs in a
# long-lived docker container; the socket dir /tmp/ub-qemu-links-eight is
# bind-mounted, making per-node consoles attachable from the host.
#
# usage:
#   boot_ub_eight_node_shell.sh up             # boot cluster (default)
#   boot_ub_eight_node_shell.sh attach nodeX   # attach to nodeX console (nodeA..nodeH)
#   boot_ub_eight_node_shell.sh status         # show nodes and socket paths
#   boot_ub_eight_node_shell.sh stop           # kill all nodes, clean sockets
#
# Must run on the Linux aarch64 verification host (n4-910c); the QEMU binary
# and openEuler artifacts (rootfs.qcow2, initramfs) must already exist —
# i.e. build_openeuler_guest.sh has been run once.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
REPO_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
LAUNCHER="guest-linux/aarch64/scripts/launch_ub_eight_node_headless.sh"
OPENEULER_GUEST_DIR="${SIM_W5_OPENEULER_GUEST_DIR:-$ROOT_DIR/out/openeuler_guest}"
ENV_DIR="$OPENEULER_GUEST_DIR/env"
STATE_FILE="$ROOT_DIR/out/ub_eight_node_shell.env"
CONSOLE_PY="$ROOT_DIR/out/ub_serial_console.py"
IMAGE="${SIM_W5_SHELL_IMAGE:-openeuler-2403:v0.0.4}"
CONTAINER="${SIM_W5_SHELL_CONTAINER:-ub-shell-8node}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-eight}"

SCRIPT_NAME="${${(%):-%x}:t}"
NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)

usage() {
  cat >&2 <<'USAGE'
usage:
  boot_ub_eight_node_shell.sh up             # boot 8-node openEuler UB cluster (default)
  boot_ub_eight_node_shell.sh attach nodeX   # attach to nodeX console (nodeA..nodeH)
  boot_ub_eight_node_shell.sh status         # show nodes and socket paths
  boot_ub_eight_node_shell.sh stop           # kill all nodes, clean sockets

Boots the 8-node UB-interconnected openEuler cluster for INTERACTIVE use and
leaves it running: each node gets a root shell on its serial console (unlike
run_w5_in_container.sh, which drives the W5 workload and tears down).
Must run on the Linux aarch64 verification host (n4-910c) after
build_openeuler_guest.sh has produced out/openeuler_guest/rootfs.qcow2.
USAGE
}

die() { echo "error: $*" >&2; exit 1; }

write_console_helper() {
  cat > "$CONSOLE_PY" <<'PY'
#!/usr/bin/env python3
# Minimal serial-console attach: bridges stdio to a QEMU unix-socket chardev.
# Ctrl-] detaches.
import os, select, socket, sys, termios, tty

if len(sys.argv) != 2:
    sys.stderr.write("usage: ub_serial_console.py <serial.sock>\n")
    sys.exit(2)

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
fd = sys.stdin.fileno()
old = termios.tcgetattr(fd)
tty.setraw(fd)
try:
    while True:
        r, _, _ = select.select([fd, s], [], [])
        if fd in r:
            data = os.read(fd, 4096)
            if not data or b"\x1d" in data:  # EOF or Ctrl-]
                break
            s.sendall(data)
        if s in r:
            data = s.recv(4096)
            if not data:
                break
            os.write(sys.stdout.fileno(), data)
finally:
    termios.tcsetattr(fd, termios.TCSADRAIN, old)
PY
  chmod +x "$CONSOLE_PY"
}

write_guest_env() {
  mkdir -p "$ENV_DIR"
  # Stale env.sh/runner.sh may be root-owned (W5 runs write them from docker);
  # unlink first so we can recreate them as the current user.
  rm -f "$ENV_DIR/env.sh" "$ENV_DIR/runner.sh"
  cat > "$ENV_DIR/env.sh" <<'EOF'
# Shell-mode per-run env (written by boot_ub_eight_node_shell.sh).
export UB_SHELL_MODE=1
EOF
  # runner.sh: label the node from its UB IP, then hand the serial console to
  # an interactive root bash. setsid -c gives bash a controlling tty (job
  # control); fall back to a plain redirect if setsid is unavailable.
  cat > "$ENV_DIR/runner.sh" <<'EOF'
#!/bin/sh
ip=$(sed -n 's/.*linqu_ipourma_ipv4=\([0-9.]*\).*/\1/p' /proc/cmdline)
n=${ip##*.}
# the minimal image lacks the hostname(1) binary; write the kernel directly
[ -n "$n" ] && echo "ub-node-$n" > /proc/sys/kernel/hostname 2>/dev/null
echo "[shell-mode] node up: hostname=$(cat /proc/sys/kernel/hostname) ub_ip=$ip"
echo "[shell-mode] interactive root bash on this console; Ctrl-] detaches the host side"
if command -v setsid >/dev/null 2>&1; then
  setsid -c /bin/bash -l </dev/ttyAMA0 >/dev/ttyAMA0 2>&1
else
  exec /bin/bash -i </dev/ttyAMA0 >/dev/ttyAMA0 2>&1
fi
EOF
  chmod +x "$ENV_DIR/runner.sh"  # 9p security_model=none passes host modes through
}

cmd_up() {
  [[ "$(uname -s)" == "Linux" && "$(uname -m)" == "aarch64" ]] || \
    die "must run on the Linux aarch64 verification host (n4-910c)"
  [[ -f "$OPENEULER_GUEST_DIR/rootfs.qcow2" ]] || \
    die "openEuler rootfs missing: $OPENEULER_GUEST_DIR/rootfs.qcow2 (run build_openeuler_guest.sh first)"
  command -v docker >/dev/null || die "docker not found"
  if docker ps -a --format '{{.Names}}' | grep -qx "$CONTAINER"; then
    die "container $CONTAINER already exists (stop first: $SCRIPT_NAME stop)"
  fi
  if pgrep -f "ub-cluster-mode=on" >/dev/null 2>&1; then
    die "UB cluster QEMU processes already running (stop them first: $SCRIPT_NAME stop)"
  fi

  write_console_helper
  write_guest_env
  mkdir -p "$SHARED_DIR"

  echo "[shell-mode] starting container $CONTAINER ($IMAGE)..."
  docker run -d --name "$CONTAINER" --privileged --network host \
    -v "$REPO_ROOT:/work" -w /work \
    -v "$SHARED_DIR:$SHARED_DIR" \
    "$IMAGE" tail -f /dev/null >/dev/null
  # Runtime libs the prebuilt QEMU links against (liburing/pixman, plus the
  # GTK stack because the current n4 build was configured with GTK enabled)
  # live in the image's dnf repo, not the host; install the runtime packages.
  docker exec "$CONTAINER" sh -c \
    'ldd /work/vendor/qemu_8.2.0_ub/build/qemu-system-aarch64 2>/dev/null | grep -q "not found" && dnf install -y -q pixman liburing libepoxy gtk3 cairo gdk-pixbuf2 libX11 || true'
  # Exception: the prebuilt binary links the HOST's liburing.so.1, but the
  # image's dnf repo only ships liburing 2.4 (liburing.so.2). Copy the host's
  # .so.1 into the container when the binary cannot find it there.
  if docker exec "$CONTAINER" sh -c \
    'ldd /work/vendor/qemu_8.2.0_ub/build/qemu-system-aarch64 2>/dev/null | grep -q "liburing.so.1 => not found"'; then
    host_liburing="$(ldconfig -p | awk '/liburing\.so\.1 \(/{print $NF; exit}')"
    if [[ -n "$host_liburing" && -e "$host_liburing" ]]; then
      docker cp -L "$host_liburing" "$CONTAINER:/usr/lib64/liburing.so.1" >/dev/null
      docker exec "$CONTAINER" ldconfig
    else
      echo "[shell-mode] warning: host liburing.so.1 not found; prebuilt QEMU may fail to start" >&2
    fi
  fi

  echo "[shell-mode] launching 8-node openEuler cluster (UB full-mesh)..."
  # UB_USE_PREBUILT_QEMU=1 / UB_SYNC_ARTIFACTS=0: reuse the verified prebuilt
  # QEMU binary and guest artifacts; never rebuild from this host-side flow.
  local env_file_container env_file
  env_file_container=$(docker exec "$CONTAINER" zsh -c \
    "cd /work && UB_USE_PREBUILT_QEMU=1 UB_SYNC_ARTIFACTS=0 SIM_W5_GUEST_BACKEND=openeuler zsh $LAUNCHER" 2>&1 | tee /dev/stderr | tail -1)
  env_file="${env_file_container/#\/work/$REPO_ROOT}"
  [[ -f "$env_file" ]] || die "launcher did not produce an env file (got: $env_file_container)"
  cp "$env_file" "$STATE_FILE"
  # Serial sockets are created by root-owned QEMU; open them to the host user.
  docker exec "$CONTAINER" sh -c "chmod a+rw '$SHARED_DIR'/serial/*.sock" 2>/dev/null || true
  echo "[shell-mode] cluster is up. Guest boot takes ~30-60s; press Enter after attach."
  cmd_status
}

socket_for() {
  local node="$1"
  [[ -f "$STATE_FILE" ]] || die "no cluster state ($STATE_FILE); run: $SCRIPT_NAME up"
  local var="${node:u}_SERIAL_SOCKET"
  local sock
  sock=$(source "$STATE_FILE" >/dev/null 2>&1 && eval "echo \${$var:-}")
  [[ -n "$sock" && -S "$sock" ]] || die "no live socket for $node (stale state? try: $SCRIPT_NAME up)"
  echo "$sock"
}

cmd_attach() {
  local node="${1:-}"
  [[ -n "$node" ]] || die "usage: $SCRIPT_NAME attach nodeX (nodeA..nodeH)"
  write_console_helper
  local sock
  sock=$(socket_for "$node")
  echo "[attach] $node -> $sock (Ctrl-] detaches)"
  exec python3 "$CONSOLE_PY" "$sock"
}

cmd_status() {
  [[ -f "$STATE_FILE" ]] || die "no cluster state; run: $SCRIPT_NAME up"
  ( source "$STATE_FILE" >/dev/null 2>&1
    echo "run_id=$RUN_ID  logs=$RUN_DIR"
    for node in "${NODE_IDS[@]}"; do
      local var="${node:u}_SERIAL_SOCKET"
      eval "local sock=\${$var:-}"
      local live=dead; [[ -S "$sock" ]] && live=live
      printf "  %-6s %-4s %s\n" "$node" "$live" "$sock"
    done )
  echo "attach:  $SCRIPT_NAME attach nodeA"
  echo "stop:    $SCRIPT_NAME stop"
}

cmd_stop() {
  local cleanup=""
  if [[ -f "$STATE_FILE" ]]; then
    cleanup=$(source "$STATE_FILE" >/dev/null 2>&1 && echo "${CLEANUP_SCRIPT:-}")
  fi
  local found=0
  if docker ps -a --format '{{.Names}}' 2>/dev/null | grep -qx "$CONTAINER"; then
    found=1
    # QEMU runs as root inside the container; cleanup must run there too.
    if [[ -n "$cleanup" ]]; then
      docker exec "$CONTAINER" bash "$cleanup" >/dev/null 2>&1 || true
    fi
    docker rm -f "$CONTAINER" >/dev/null
  fi
  (( found )) || [[ -f "$STATE_FILE" ]] || die "no cluster state; nothing to stop"
  rm -f "$STATE_FILE"
  echo "[shell-mode] cluster stopped"
}

case "${1:-up}" in
  up)            cmd_up ;;
  attach)        shift; cmd_attach "$@" ;;
  status)        cmd_status ;;
  stop)          cmd_stop ;;
  -h|--help)     usage ;;
  *)             usage >&2; exit 2 ;;
esac
