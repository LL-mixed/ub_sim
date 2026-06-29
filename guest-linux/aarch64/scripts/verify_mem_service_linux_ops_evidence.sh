#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_APP_DIR="$ROOT_DIR/apps/mem_service"
APP_DIR=""
HOST_BIN=""
EVIDENCE_FILE=""
DRY_RUN=0

usage() {
  cat <<'EOF'
Usage: verify_mem_service_linux_ops_evidence.sh --evidence-file PATH [--app-dir DIR] [--dry-run]

Verifies a mem_service real-Linux ops certification evidence artifact.

Options:
  --evidence-file PATH  Evidence file produced by run_mem_service_linux_ops_ci.sh.
  --app-dir DIR         mem_service app directory containing linqu_mem_service_host.
                        By default the script first tries the installed
                        libexec binary next to the installed share tree, then
                        falls back to the source-tree app directory.
  --dry-run             Print the build and verify commands without running them.
  -h, --help            Show this help.
EOF
}

resolve_host_bin() {
  local installed_host

  if [[ -n "$APP_DIR" ]]; then
    HOST_BIN="$APP_DIR/linqu_mem_service_host"
    if [[ ! -x "$HOST_BIN" ]]; then
      make -C "$APP_DIR" linqu_mem_service_host
    fi
    return
  fi

  installed_host="$(cd "$SCRIPT_DIR/../../../.." && pwd)/libexec/lingqu/mem_service/linqu_mem_service_host"
  if [[ -x "$installed_host" ]]; then
    HOST_BIN="$installed_host"
    return
  fi

  APP_DIR="$DEFAULT_APP_DIR"
  HOST_BIN="$APP_DIR/linqu_mem_service_host"
  if [[ ! -d "$APP_DIR" ]]; then
    echo "[mem-service-linux-ops-evidence] FAIL: app directory not found: $APP_DIR" >&2
    exit 1
  fi
  if [[ ! -x "$HOST_BIN" ]]; then
    make -C "$APP_DIR" linqu_mem_service_host
  fi
}

print_host_verify_commands() {
  local installed_host

  if [[ -n "$APP_DIR" ]]; then
    printf 'make -C %s linqu_mem_service_host\n' "$APP_DIR"
    printf '%s/linqu_mem_service_host ops-certification-verify --evidence-file %s\n' "$APP_DIR" "$EVIDENCE_FILE"
    return
  fi

  installed_host="$(cd "$SCRIPT_DIR/../../../.." && pwd)/libexec/lingqu/mem_service/linqu_mem_service_host"
  if [[ -x "$installed_host" ]]; then
    printf '%s ops-certification-verify --evidence-file %s\n' "$installed_host" "$EVIDENCE_FILE"
    return
  fi

  printf 'make -C %s linqu_mem_service_host\n' "$DEFAULT_APP_DIR"
  printf '%s/linqu_mem_service_host ops-certification-verify --evidence-file %s\n' "$DEFAULT_APP_DIR" "$EVIDENCE_FILE"
}

while (( $# > 0 )); do
  case "$1" in
    --evidence-file)
      if (( $# < 2 )); then
        echo "--evidence-file requires a path" >&2
        exit 2
      fi
      EVIDENCE_FILE="$2"
      shift 2
      ;;
    --app-dir)
      if (( $# < 2 )); then
        echo "--app-dir requires a path" >&2
        exit 2
      fi
      APP_DIR="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unexpected argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$EVIDENCE_FILE" ]]; then
  echo "[mem-service-linux-ops-evidence] FAIL: --evidence-file is required" >&2
  exit 2
fi

if [[ "$DRY_RUN" == "1" ]]; then
  print_host_verify_commands
  exit 0
fi

if [[ ! -f "$EVIDENCE_FILE" ]]; then
  echo "[mem-service-linux-ops-evidence] FAIL: evidence file not found: $EVIDENCE_FILE" >&2
  exit 1
fi

resolve_host_bin
"$HOST_BIN" ops-certification-verify --evidence-file "$EVIDENCE_FILE"
printf '[mem-service-linux-ops-evidence] PASS evidence=%s\n' "$EVIDENCE_FILE"
