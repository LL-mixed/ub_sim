#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
MEM_SERVICE_APP_DIR="$ROOT_DIR/apps/mem_service"
MEM_SERVICE_HOST_BIN="$MEM_SERVICE_APP_DIR/linqu_mem_service_host"

usage() {
  cat >&2 <<'USAGE'
usage: run_w5_memory_service_bootstrap.sh [--print-env] [--env-file FILE]

Prepares the host-side W5 Memory Service runtime surface independently from
the infer execution path. The caller should source the generated env file
before launching W5 infer or serving queue workers.
USAGE
}

PRINT_ENV=0
ENV_FILE=""

while (( $# > 0 )); do
  case "$1" in
    --print-env)
      PRINT_ENV=1
      shift
      ;;
    --env-file)
      if (( $# < 2 )); then
        echo "--env-file requires a value" >&2
        usage
        exit 2
      fi
      ENV_FILE="$2"
      shift 2
      ;;
    --env-file=*)
      ENV_FILE="${1#--env-file=}"
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

SIM_UAPI_W5_PROFILE="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w5_${SIM_UAPI_W5_PROFILE}_${RANDOM}}"
SIM_W5_MEMORY_STORE="${SIM_W5_MEMORY_STORE:-$OUT_DIR/w5_memory_object_store.${RUN_ID}.json}"
SIM_W5_MEMORY_OBJECT_STORE="${SIM_W5_MEMORY_OBJECT_STORE:-$OUT_DIR/w5_object_service_store.${RUN_ID}.json}"
SIM_W5_MEMORY_ENGRAM_STATE="${SIM_W5_MEMORY_ENGRAM_STATE:-$OUT_DIR/w5_memory_engram_state.${RUN_ID}.json}"
SIM_W5_MEMORY_REGISTRY_DIR="${SIM_W5_MEMORY_REGISTRY_DIR:-$OUT_DIR/w5_memory_registry.${RUN_ID}}"
SIM_W5_MEMORY_OWNER_ENTITY="${SIM_W5_MEMORY_OWNER_ENTITY:-0}"
SIM_W5_MEMORY_PRODUCER_ENTITY="${SIM_W5_MEMORY_PRODUCER_ENTITY:-0}"
SIM_W5_MEMORY_SERVICE="${SIM_W5_MEMORY_SERVICE:-lingqu_memory_service}"

case "$SIM_UAPI_W5_PROFILE" in
  qwen3_0_6b_decode|qwen3_14b_decode|qwen3_0_6b_engram_decode|qwen3_14b_engram_decode)
    ;;
  *)
    echo "unsupported SIM_UAPI_W5_PROFILE=$SIM_UAPI_W5_PROFILE" >&2
    exit 2
    ;;
esac

if [[ ! -x "$MEM_SERVICE_HOST_BIN" ]]; then
  echo "[w5_memory_service_bootstrap] build mem_service host binary: $MEM_SERVICE_HOST_BIN" >&2
  make -C "$MEM_SERVICE_APP_DIR" linqu_mem_service_host >&2
fi
if [[ ! -x "$MEM_SERVICE_HOST_BIN" ]]; then
  echo "W5 Memory Service bootstrap requires mem_service host binary: $MEM_SERVICE_HOST_BIN" >&2
  exit 2
fi

if [[ -z "$ENV_FILE" ]]; then
  ENV_FILE="$OUT_DIR/w5_memory_service_env.${RUN_ID}.sh"
fi
mkdir -p "${ENV_FILE:h}"

tmp_file="$ENV_FILE.tmp.$$"
"$MEM_SERVICE_HOST_BIN" bootstrap-w5-service \
  --memory-store "$SIM_W5_MEMORY_STORE" \
  --memory-object-store "$SIM_W5_MEMORY_OBJECT_STORE" \
  --memory-engram-state "$SIM_W5_MEMORY_ENGRAM_STATE" \
  --memory-registry-dir "$SIM_W5_MEMORY_REGISTRY_DIR" \
  --owner-entity "$SIM_W5_MEMORY_OWNER_ENTITY" \
  --producer-entity "$SIM_W5_MEMORY_PRODUCER_ENTITY" \
  --service-name "$SIM_W5_MEMORY_SERVICE" \
  --print-env >"$tmp_file"
mv "$tmp_file" "$ENV_FILE"

if (( PRINT_ENV )); then
  cat "$ENV_FILE"
else
  echo "W5 Memory Service bootstrap env: $ENV_FILE" >&2
fi
