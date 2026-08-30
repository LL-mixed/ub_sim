#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GUEST_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$GUEST_ROOT/../.." && pwd)"
GENERIC_RUNNER="$SCRIPT_DIR/run_ub_dual_node_apps.sh"
MANIFEST=""
SCENARIO="$WORKSPACE_ROOT/scenarios/mvp_2host_single_domain.yaml"
SIM_CLI_BIN="${SIM_CLI_BIN:-$WORKSPACE_ROOT/target/release/sim-cli}"
KERNEL_IMAGE="${KERNEL_IMAGE:-$GUEST_ROOT/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$GUEST_ROOT/out/initramfs.cpio.gz}"
ELEMENTS=16384
GENERATION=101
TOKEN_VALUE=0
TIMEOUT_MS=120000
NODEA_CNA=0xf001
NODEB_CNA=0xf002
AUTHORIZATION_DELAY_NS=0
AUTHORIZATION_TIMEOUT_NS=1000000000
CANCEL_AFTER_MS=0
RESET_ON_PENDING=0
INJECT_DUPLICATE_COMPLETION=0
INJECT_LATE_COMPLETION=0
EXPECT="success"
RUN_SECS=180
MAX_RUNTIME=300
RUN_ID="lingqu-shmem-pto-$(date +%Y%m%dT%H%M%S)-${RANDOM}"
EVIDENCE_DIR=""

usage() {
  cat <<'USAGE'
Usage: run_ub_dual_node_lingqu_shmem_pto_direct.sh [options]

Required:
  --manifest PATH        Simpler host-vector artifact manifest.

Options:
  --scenario PATH        Two-host Rust simulator scenario.
  --sim-cli-bin PATH     sim-cli containing the fingerprint query.
  --kernel-image PATH    Arm64 guest kernel image.
  --initramfs-image PATH Guest initramfs with the PTO workload.
  --elements N           Callable 1 element count; must be 16384.
  --generation N         OBMM bootstrap generation.
  --token-value N        OBMM import token value.
  --timeout-ms N         Guest producer/dispatch timeout.
  --nodea-cna N          QEMU PTO device CNA for nodeA.
  --nodeb-cna N          QEMU PTO device/requester CNA for nodeB.
  --authorization-delay-ns N
                         Per-memref QEMU authorization delay; zero is sync.
  --authorization-timeout-ns N
                         Dispatch-wide authorization timeout.
  --cancel-after-ms N    Cancel pending authorization after N guest ms.
  --reset-on-pending 0|1
                         Reset nodeB through QMP after authorization suspends.
  --inject-duplicate-completion 0|1
                         Inject a duplicate timer completion.
  --inject-late-completion 0|1
                         Inject a completion after cancel cleanup.
  --expect OUTCOME       Expected result: success, authorization-timeout,
                         or authorization-cancelled.
  --run-secs N           Harness per-app timeout.
  --max-runtime N        Harness global watchdog timeout.
  --run-id ID            Stable evidence and log identifier.
  --evidence-dir PATH    New, non-existing evidence directory.
  -h, --help             Show this help.
USAGE
}

require_value() {
  local option="$1"
  local count="$2"

  if [[ "$count" -lt 2 ]]; then
    echo "$option requires a value" >&2
    usage >&2
    exit 2
  fi
}

canonical_file() {
  local label="$1"
  local input_path="$2"

  if [[ ! -f "$input_path" ]]; then
    echo "$label does not exist: $input_path" >&2
    exit 2
  fi
  printf '%s/%s\n' \
    "$(cd "$(dirname "$input_path")" && pwd)" \
    "$(basename "$input_path")"
}

hash_file() {
  local input_path="$1"

  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$input_path"
  else
    shasum -a 256 "$input_path"
  fi
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest)
      require_value "$1" "$#"
      MANIFEST="$2"
      shift 2
      ;;
    --scenario)
      require_value "$1" "$#"
      SCENARIO="$2"
      shift 2
      ;;
    --sim-cli-bin)
      require_value "$1" "$#"
      SIM_CLI_BIN="$2"
      shift 2
      ;;
    --kernel-image)
      require_value "$1" "$#"
      KERNEL_IMAGE="$2"
      shift 2
      ;;
    --initramfs-image)
      require_value "$1" "$#"
      INITRAMFS_IMAGE="$2"
      shift 2
      ;;
    --elements)
      require_value "$1" "$#"
      ELEMENTS="$2"
      shift 2
      ;;
    --generation)
      require_value "$1" "$#"
      GENERATION="$2"
      shift 2
      ;;
    --token-value)
      require_value "$1" "$#"
      TOKEN_VALUE="$2"
      shift 2
      ;;
    --timeout-ms)
      require_value "$1" "$#"
      TIMEOUT_MS="$2"
      shift 2
      ;;
    --nodea-cna)
      require_value "$1" "$#"
      NODEA_CNA="$2"
      shift 2
      ;;
    --nodeb-cna)
      require_value "$1" "$#"
      NODEB_CNA="$2"
      shift 2
      ;;
    --authorization-delay-ns)
      require_value "$1" "$#"
      AUTHORIZATION_DELAY_NS="$2"
      shift 2
      ;;
    --authorization-timeout-ns)
      require_value "$1" "$#"
      AUTHORIZATION_TIMEOUT_NS="$2"
      shift 2
      ;;
    --cancel-after-ms)
      require_value "$1" "$#"
      CANCEL_AFTER_MS="$2"
      shift 2
      ;;
    --reset-on-pending)
      require_value "$1" "$#"
      RESET_ON_PENDING="$2"
      shift 2
      ;;
    --inject-duplicate-completion)
      require_value "$1" "$#"
      INJECT_DUPLICATE_COMPLETION="$2"
      shift 2
      ;;
    --inject-late-completion)
      require_value "$1" "$#"
      INJECT_LATE_COMPLETION="$2"
      shift 2
      ;;
    --expect)
      require_value "$1" "$#"
      EXPECT="$2"
      shift 2
      ;;
    --run-secs)
      require_value "$1" "$#"
      RUN_SECS="$2"
      shift 2
      ;;
    --max-runtime)
      require_value "$1" "$#"
      MAX_RUNTIME="$2"
      shift 2
      ;;
    --run-id)
      require_value "$1" "$#"
      RUN_ID="$2"
      shift 2
      ;;
    --evidence-dir)
      require_value "$1" "$#"
      EVIDENCE_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$MANIFEST" ]]; then
  echo "--manifest is required" >&2
  usage >&2
  exit 2
fi
if [[ -z "$RUN_ID" || "$RUN_ID" == *[^A-Za-z0-9._-]* ]]; then
  echo "run id must contain only ASCII letters, digits, dots, dashes, or underscores" >&2
  exit 2
fi
case "$EXPECT" in
  success|authorization-timeout|authorization-cancelled)
    ;;
  *)
    echo "expected result must be success, authorization-timeout, or authorization-cancelled" >&2
    exit 2
    ;;
esac
if [[ ! -x "$SIM_CLI_BIN" &&
      "$SIM_CLI_BIN" == "$WORKSPACE_ROOT/target/release/sim-cli" &&
      -x "$WORKSPACE_ROOT/target/debug/sim-cli" ]]; then
  SIM_CLI_BIN="$WORKSPACE_ROOT/target/debug/sim-cli"
fi
if [[ ! -x "$SIM_CLI_BIN" ]]; then
  echo "sim-cli is missing or not executable: $SIM_CLI_BIN" >&2
  echo "build it with: cargo build --release -p sim-cli" >&2
  exit 2
fi
if [[ ! -x "$GENERIC_RUNNER" ]]; then
  echo "generic dual-node runner is not executable: $GENERIC_RUNNER" >&2
  exit 2
fi

MANIFEST="$(canonical_file "PTO manifest" "$MANIFEST")"
SCENARIO="$(canonical_file "simulator scenario" "$SCENARIO")"
SIM_CLI_BIN="$(canonical_file "sim-cli" "$SIM_CLI_BIN")"
KERNEL_IMAGE="$(canonical_file "kernel image" "$KERNEL_IMAGE")"
INITRAMFS_IMAGE="$(canonical_file "initramfs image" "$INITRAMFS_IMAGE")"
if [[ -z "$EVIDENCE_DIR" ]]; then
  EVIDENCE_DIR="$WORKSPACE_ROOT/out/lingqu-shmem-pto-e2e/$RUN_ID"
fi
if [[ -e "$EVIDENCE_DIR" ]]; then
  echo "evidence directory already exists: $EVIDENCE_DIR" >&2
  exit 2
fi
mkdir -p "$EVIDENCE_DIR"
EVIDENCE_DIR="$(cd "$EVIDENCE_DIR" && pwd)"

FINGERPRINT_JSON="$EVIDENCE_DIR/callable-fingerprint.json"
"$SIM_CLI_BIN" lingqu-shmem-pto-e2e \
  --fingerprint-manifest "$MANIFEST" > "$FINGERPRINT_JSON"
ARTIFACT_FINGERPRINT="$(python3 - "$FINGERPRINT_JSON" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    payload = json.load(stream)
if payload.get("command") != "lingqu-shmem-pto-e2e":
    raise SystemExit("unexpected fingerprint command")
if payload.get("implementation_phase") != "p3_guest_runtime":
    raise SystemExit("unexpected fingerprint phase")
if payload.get("callable_id") != 1:
    raise SystemExit("unexpected callable id")
value = payload.get("artifact_fingerprint")
hex_value = payload.get("artifact_fingerprint_hex")
if not isinstance(value, int) or value <= 0:
    raise SystemExit("invalid artifact fingerprint")
if hex_value != f"0x{value:016x}":
    raise SystemExit("inconsistent artifact fingerprint encodings")
print(hex_value)
PY
)"

REPORT_FILE="$EVIDENCE_DIR/apps-report.txt"
HARNESS_LOG="$EVIDENCE_DIR/harness.log"
QMP_ARGS=()
if [[ "$RESET_ON_PENDING" != "0" ]]; then
  QMP_ARGS=(--use-qmp)
fi
set +e
"$GENERIC_RUNNER" \
  --app lingqu_shmem_pto_direct \
  --run-id "$RUN_ID" \
  --run-secs "$RUN_SECS" \
  --max-runtime "$MAX_RUNTIME" \
  --report-file "$REPORT_FILE" \
  --kernel-image "$KERNEL_IMAGE" \
  --initramfs-image "$INITRAMFS_IMAGE" \
  --use-prebuilt-qemu \
  --pto-manifest "$MANIFEST" \
  --pto-scenario "$SCENARIO" \
  --pto-artifact-fingerprint "$ARTIFACT_FINGERPRINT" \
  --pto-elements "$ELEMENTS" \
  --pto-generation "$GENERATION" \
  --pto-token-value "$TOKEN_VALUE" \
  --pto-timeout-ms "$TIMEOUT_MS" \
  --pto-nodea-cna "$NODEA_CNA" \
  --pto-nodeb-cna "$NODEB_CNA" \
  --pto-authorization-delay-ns "$AUTHORIZATION_DELAY_NS" \
  --pto-authorization-timeout-ns "$AUTHORIZATION_TIMEOUT_NS" \
  --pto-cancel-after-ms "$CANCEL_AFTER_MS" \
  --pto-reset-on-pending "$RESET_ON_PENDING" \
  --pto-inject-duplicate-completion "$INJECT_DUPLICATE_COMPLETION" \
  --pto-inject-late-completion "$INJECT_LATE_COMPLETION" \
  --pto-expect "$EXPECT" \
  "${QMP_ARGS[@]}" \
  > "$HARNESS_LOG" 2>&1
RUNNER_RC=$?
set -e
cat "$HARNESS_LOG"

for log_dir in "$GUEST_ROOT/logs/${RUN_ID}_apps_iter"*; do
  cp -R "$log_dir" "$EVIDENCE_DIR/"
done

ARTIFACT_LIST="$EVIDENCE_DIR/artifact-paths.txt"
python3 - "$MANIFEST" <<'PY' > "$ARTIFACT_LIST"
import json
import os
import sys

with open(sys.argv[1], "r", encoding="utf-8") as stream:
    runtime = json.load(stream)["simpler_runtime"]
paths = []
for key in ("host_runtime_library", "orch_shared_object", "aicpu_binary", "aicore_binary"):
    item = runtime.get(key)
    if item:
        paths.append(item["source"])
for kernel in runtime.get("kernels", []):
    paths.append(kernel["binary"]["source"])
for value in runtime.get("runtime_env", {}).values():
    if isinstance(value, str) and os.path.isfile(value):
        paths.append(value)
for path in paths:
    print(path)
PY

HASH_FILE="$EVIDENCE_DIR/sha256.txt"
QEMU_BINARY="$WORKSPACE_ROOT/vendor/qemu_8.2.0_ub/build/qemu-system-aarch64"
{
  hash_file "$MANIFEST"
  hash_file "$SCENARIO"
  hash_file "$SIM_CLI_BIN"
  hash_file "$KERNEL_IMAGE"
  hash_file "$INITRAMFS_IMAGE"
  if [[ -f "$QEMU_BINARY" ]]; then
    hash_file "$QEMU_BINARY"
  else
    echo "MISSING  $QEMU_BINARY"
  fi
  while IFS= read -r artifact; do
    if [[ -f "$artifact" ]]; then
      hash_file "$artifact"
    else
      echo "MISSING  $artifact"
    fi
  done < "$ARTIFACT_LIST"
} > "$HASH_FILE"

SOURCE_HASH_FILE="$EVIDENCE_DIR/source-sha256.txt"
{
  hash_file "$GENERIC_RUNNER"
  hash_file "$0"
  hash_file "$GUEST_ROOT/apps/lingqu_shmem_pto_direct/lingqu_shmem_pto_direct.c"
  hash_file "$GUEST_ROOT/initramfs/run_app"
  hash_file "$GUEST_ROOT/libs/lingqu_shmem_pto/lingqu_shmem_pto_endpoint.c"
  hash_file "$GUEST_ROOT/libs/lingqu_shmem_pto/lingqu_shmem_pto_endpoint.h"
  hash_file "$WORKSPACE_ROOT/crates/sim-qemu/include/linqu_shmem_pto_abi.h"
  hash_file "$WORKSPACE_ROOT/crates/sim-qemu/src/ub_gm_abi.rs"
  hash_file "$WORKSPACE_ROOT/vendor/qemu_8.2.0_ub/hw/ub/ub_ubc.c"
  hash_file "$WORKSPACE_ROOT/vendor/qemu_8.2.0_ub/include/hw/ub/ub_ubc.h"
} > "$SOURCE_HASH_FILE"

{
  echo "root_commit=$(git -C "$WORKSPACE_ROOT" rev-parse HEAD)"
  echo "root_status_begin"
  git -C "$WORKSPACE_ROOT" status --short --ignore-submodules=none
  echo "root_status_end"
  echo "submodules_begin"
  git -C "$WORKSPACE_ROOT" submodule status --recursive 2>&1 || true
  echo "submodules_end"
} > "$EVIDENCE_DIR/revisions.txt"

QEMU_LEFTOVERS="$EVIDENCE_DIR/qemu-leftovers.txt"
pgrep -af '[q]emu-system-aarch64' > "$QEMU_LEFTOVERS" 2>/dev/null || true
if [[ -s "$QEMU_LEFTOVERS" && "$RUNNER_RC" -eq 0 ]]; then
  echo "QEMU processes remain after validation; preserving them for audit" >&2
  RUNNER_RC=31
fi

{
  if [[ "$RUNNER_RC" -eq 0 ]]; then
    echo "validation.status=pass"
  else
    echo "validation.status=fail"
  fi
  echo "runner_exit_code=$RUNNER_RC"
  echo "run_id=$RUN_ID"
  echo "evidence_dir=$EVIDENCE_DIR"
  echo "manifest=$MANIFEST"
  echo "scenario=$SCENARIO"
  echo "artifact_fingerprint=$ARTIFACT_FINGERPRINT"
  echo "elements=$ELEMENTS"
  echo "generation=$GENERATION"
  echo "nodea_cna=$NODEA_CNA"
  echo "nodeb_cna=$NODEB_CNA"
  echo "authorization_delay_ns=$AUTHORIZATION_DELAY_NS"
  echo "authorization_timeout_ns=$AUTHORIZATION_TIMEOUT_NS"
  echo "cancel_after_ms=$CANCEL_AFTER_MS"
  echo "reset_on_pending=$RESET_ON_PENDING"
  echo "inject_duplicate_completion=$INJECT_DUPLICATE_COMPLETION"
  echo "inject_late_completion=$INJECT_LATE_COMPLETION"
  echo "expected_result=$EXPECT"
  echo "qemu_binary=$QEMU_BINARY"
} > "$EVIDENCE_DIR/validation.status"

echo "PTO UB_GM evidence: $EVIDENCE_DIR"
exit "$RUNNER_RC"
