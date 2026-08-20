#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

IMAGE="openeuler-2403:v0.0.4"
DRY_RUN=0
RECONFIGURE_QEMU=0
HOST_OS="$(uname -s 2>/dev/null || echo unknown)"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_in_container.sh [--image IMAGE] [--reconfigure-qemu] [--dry-run] -- W5_ARGS...
       run_w5_in_container.sh [--image IMAGE] [--reconfigure-qemu] [--dry-run] W5_ARGS...

Runs W5 from the host. On macOS it delegates directly to the local W5 runner;
on Linux it enters a Docker container, prepares native build dependencies,
builds the workspace QEMU when needed, and delegates to:
  ./guest-linux/aarch64/scripts/run_w5_cluster_config.sh W5_ARGS...

Example:
  ./guest-linux/aarch64/scripts/run_w5_in_container.sh w5.env

For serving requests:
  ./guest-linux/aarch64/scripts/run_w5_in_container.sh \
    -- --serve-requests requests.txt --nodea-ingress w5.env
USAGE
}

while (( $# > 0 )); do
  case "$1" in
    --image)
      if (( $# < 2 )); then
        echo "--image requires a value" >&2
        usage
        exit 2
      fi
      IMAGE="$2"
      shift 2
      ;;
    --image=*)
      IMAGE="${1#--image=}"
      shift
      ;;
    --reconfigure-qemu)
      RECONFIGURE_QEMU=1
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --)
      shift
      break
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      break
      ;;
  esac
done

if (( $# == 0 )); then
  echo "missing W5 arguments" >&2
  usage
  exit 2
fi

if [[ "$HOST_OS" == "Darwin" ]]; then
  if (( DRY_RUN )); then
    if (( RECONFIGURE_QEMU )); then
      printf 'RECONFIGURE=1 %q && ' "$SCRIPT_DIR/build_qemu_binary.sh"
    fi
    printf '%q ' "$SCRIPT_DIR/run_w5_cluster_config.sh" "$@"
    printf '\n'
    exit 0
  fi
  if (( RECONFIGURE_QEMU )); then
    (
      cd "$REPO_ROOT"
      RECONFIGURE=1 "$SCRIPT_DIR/build_qemu_binary.sh" >/dev/null
    )
  fi
  exec "$SCRIPT_DIR/run_w5_cluster_config.sh" "$@"
fi

docker_args=(
  run
  --rm
  --privileged
  --network
  host
  -v
  "$REPO_ROOT:/work"
  -w
  /work
)

if [[ -t 0 && -t 1 ]]; then
  docker_args=(-it "${docker_args[@]}")
fi

if [[ -d /home/ll/models ]]; then
  docker_args+=(-v /home/ll/models:/home/ll/models:ro)
  docker_args+=(-v /home/ll/models:/models:ro)
fi
if [[ -d /Volumes/repos/qwen3_mlx_run ]]; then
  docker_args+=(-v /Volumes/repos/qwen3_mlx_run:/Volumes/repos/qwen3_mlx_run:ro)
fi

container_script='
set -euo pipefail
reconfigure_qemu="$1"
shift

if ! command -v zsh >/dev/null 2>&1; then
  if command -v dnf >/dev/null 2>&1; then
    dnf install -y zsh
  elif command -v yum >/dev/null 2>&1; then
    yum install -y zsh
  elif command -v apt-get >/dev/null 2>&1; then
    apt-get update
    apt-get install -y zsh
  else
    echo "unsupported container: missing zsh and missing dnf, yum, or apt-get" >&2
    exit 1
  fi
fi

./guest-linux/aarch64/scripts/prepare_w5_container_deps.sh

git config --global --add safe.directory /work
git config --file /work/.gitmodules --get-regexp path |
while read -r _ submodule_path; do
  git config --global --add safe.directory "/work/$submodule_path"
done

if [[ -f guest-linux/aarch64/out/Image && -f guest-linux/aarch64/out/initramfs.cpio.gz ]]; then
  export UB_SYNC_ARTIFACTS="${UB_SYNC_ARTIFACTS:-0}"
fi

if [[ "$reconfigure_qemu" == "1" ]]; then
  rm -rf vendor/qemu_8.2.0_ub/build/pyvenv
  RECONFIGURE=1 ./guest-linux/aarch64/scripts/build_qemu_binary.sh
else
  ./guest-linux/aarch64/scripts/build_qemu_binary.sh
fi

exec ./guest-linux/aarch64/scripts/run_w5_cluster_config.sh "$@"
'

cmd=(docker "${docker_args[@]}" "$IMAGE" bash -lc "$container_script" run_w5_container "$RECONFIGURE_QEMU" "$@")

if (( DRY_RUN )); then
  printf '%q ' "${cmd[@]}"
  printf '\n'
  exit 0
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required to run W5 in a container" >&2
  exit 1
fi

exec "${cmd[@]}"
