#!/bin/zsh
set -euo pipefail

DRY_RUN=0

usage() {
  cat >&2 <<'USAGE'
usage: prepare_w5_container_deps.sh [--dry-run]

Installs the native container dependencies required before running W5 when the
workspace QEMU or ARM64 guest kernel must be built inside the container.

Supported package managers:
  - dnf/yum for openEuler, Fedora, RHEL-like containers
  - apt-get for Debian/Ubuntu containers
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage
      exit 2
      ;;
  esac
done

run_cmd() {
  print -r -- "+ $*"
  if (( DRY_RUN )); then
    return 0
  fi
  "$@"
}

python_has_distlib() {
  python3 - <<'PY' >/dev/null 2>&1
try:
    import distlib.scripts
    import distlib.version
except ImportError:
    from pip._vendor import distlib
    import pip._vendor.distlib.scripts
    import pip._vendor.distlib.version
PY
}

install_rpm_deps() {
  local installer="$1"
  run_cmd "$installer" install -y \
    bc \
    bison \
    cpio \
    elfutils-libelf-devel \
    flex \
    glib2-devel \
    liburing-devel \
    openssl-devel \
    pixman-devel \
    zlib-devel \
    pkgconf-pkg-config \
    ninja-build \
    gcc \
    gcc-c++ \
    make \
    zsh \
    python3-pip \
    rsync
}

install_deb_deps() {
  run_cmd apt-get update
  run_cmd apt-get install -y \
    bc \
    bison \
    cpio \
    flex \
    libelf-dev \
    libglib2.0-dev \
    libssl-dev \
    liburing-dev \
    libpixman-1-dev \
    zlib1g-dev \
    pkg-config \
    ninja-build \
    gcc \
    g++ \
    make \
    zsh \
    python3-pip \
    python3-distlib \
    rsync
}

install_distlib_for_current_python() {
  if (( DRY_RUN )); then
    run_cmd python3 -m pip install distlib
    return 0
  fi
  if python_has_distlib; then
    echo "[prepare_w5_container_deps] python3 already has distlib"
    return 0
  fi
  run_cmd python3 -m pip install distlib
}

verify_deps() {
  local missing=()
  local tool
  local pkg

  for tool in bc bison flex python3 pkg-config ninja gcc make rsync; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      missing+=("$tool")
    fi
  done
  if ! python_has_distlib; then
    missing+=("python3 distlib")
  fi
  if command -v pkg-config >/dev/null 2>&1; then
    for pkg in glib-2.0 libelf liburing openssl pixman-1 zlib; do
      if ! pkg-config --exists "$pkg"; then
        missing+=("pkg-config:$pkg")
      fi
    done
  fi

  if (( ${#missing[@]} > 0 )); then
    printf '[prepare_w5_container_deps] missing after install: %s\n' "${(j:, :)missing}" >&2
    exit 1
  fi
}

main() {
  if command -v dnf >/dev/null 2>&1; then
    install_rpm_deps dnf
  elif command -v yum >/dev/null 2>&1; then
    install_rpm_deps yum
  elif command -v apt-get >/dev/null 2>&1; then
    install_deb_deps
  elif (( DRY_RUN )); then
    install_rpm_deps dnf
  else
    echo "unsupported container: missing dnf, yum, or apt-get" >&2
    exit 1
  fi

  install_distlib_for_current_python
  if (( ! DRY_RUN )); then
    verify_deps
  fi
  echo "[prepare_w5_container_deps] ready"
}

main
