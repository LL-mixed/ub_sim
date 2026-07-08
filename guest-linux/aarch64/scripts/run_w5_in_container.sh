#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

IMAGE="openeuler-2403:v0.0.4"
DRY_RUN=0
RECONFIGURE_QEMU=0

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_in_container.sh [--image IMAGE] [--reconfigure-qemu] [--dry-run] -- W5_ARGS...
       run_w5_in_container.sh [--image IMAGE] [--reconfigure-qemu] [--dry-run] W5_ARGS...

Runs W5 from the host by entering a Docker container, preparing native build
dependencies, building the workspace QEMU when needed, and delegating to:
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
