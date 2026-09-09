#!/bin/zsh
set -euo pipefail
setopt null_glob

# ---------------------------------------------------------------------------
# run_ub_dual_node_obmm_test.sh
# Run the upstream obmm-test framework (gitcode obmm-ecology/obmm-test) on a
# 2-node simulated cluster. Each node boots the busybox guest, linqu_obmm_test
# brings up the ipourma datapath, starts obmm-test-conductor, and nodeA also
# runs obmm-test-orchestrator against both conductors.
#
# Required:
#   OBMM_TEST_BUILD_DIR  obmm-test aarch64 build tree containing
#                        src/conductor/obmm-test-conductor and
#                        src/orchestrator/obmm-test-orchestrator
# Optional (passed through to the guest):
#   OBMM_TEST_PORT                 conductor port (default 9981)
#   OBMM_TEST_GROUP_EXCLUDE        default framework_tests
#   OBMM_TEST_INCLUDE / OBMM_TEST_EXCLUDE / OBMM_TEST_GROUP_INCLUDE
#   OBMM_TEST_TIMEOUT_SCALE        default 5.0 (sim runs slower than hardware)
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
BASE_INITRAMFS="${BASE_INITRAMFS:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v0.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_two_node_v2_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-2}"
SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub-qemu-links-obmm-test}"
RUN_SECS="${RUN_SECS:-900}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_obmm_test_${RANDOM}}"
OBMM_TEST_PORT="${OBMM_TEST_PORT:-9981}"
OBMM_TEST_GROUP_EXCLUDE="${OBMM_TEST_GROUP_EXCLUDE:-framework_tests}"
OBMM_TEST_GROUP_INCLUDE="${OBMM_TEST_GROUP_INCLUDE:-}"
OBMM_TEST_INCLUDE="${OBMM_TEST_INCLUDE:-}"
OBMM_TEST_EXCLUDE="${OBMM_TEST_EXCLUDE:-}"
OBMM_TEST_TIMEOUT_SCALE="${OBMM_TEST_TIMEOUT_SCALE:-5.0}"
OBMM_TEST_BUILD_DIR="${OBMM_TEST_BUILD_DIR:-/sd_data/repo/obmm-test/build-aarch64}"
OBMM_TEST_ALL_IPS="${OBMM_TEST_ALL_IPS:-10.0.0.1,10.0.0.2}"
RUN_INITRAMFS_DIR="$OUT_DIR/obmm_test_initramfs.${RUN_ID}"
RUN_INITRAMFS="$OUT_DIR/initramfs.${RUN_ID}.cpio.gz"

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"

CONDUCTOR_BIN="$OBMM_TEST_BUILD_DIR/src/conductor/obmm-test-conductor"
ORCHESTRATOR_BIN="$OBMM_TEST_BUILD_DIR/src/orchestrator/obmm-test-orchestrator"
for required in "$KERNEL_IMAGE" "$BASE_INITRAMFS" "$CONDUCTOR_BIN" "$ORCHESTRATOR_BIN"; do
  if [[ ! -f "$required" ]]; then
    echo "[obmm-test] FAIL: missing $required" >&2
    echo "[obmm-test] hint: build guest artifacts and cross-build obmm-test first" >&2
    exit 1
  fi
done

QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$BASE_INITRAMFS"
if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi

mkdir -p "$LOG_DIR/${RUN_ID}"
NODEA_GUEST_LOG="$LOG_DIR/${RUN_ID}/nodeA_guest.log"
NODEB_GUEST_LOG="$LOG_DIR/${RUN_ID}/nodeB_guest.log"
NODEA_QEMU_LOG="$LOG_DIR/${RUN_ID}/nodeA_qemu.log"
NODEB_QEMU_LOG="$LOG_DIR/${RUN_ID}/nodeB_qemu.log"
NODEA_PID_FILE="$OUT_DIR/ub_nodeA.obmm_test.${RUN_ID}.pid"
NODEB_PID_FILE="$OUT_DIR/ub_nodeB.obmm_test.${RUN_ID}.pid"

# Per-run initramfs: base guest tree + the two obmm-test binaries.
rm -rf "$RUN_INITRAMFS_DIR" "$RUN_INITRAMFS"
mkdir -p "$RUN_INITRAMFS_DIR"
(
  cd "$RUN_INITRAMFS_DIR"
  gzip -dc "$BASE_INITRAMFS" | cpio -id --quiet
)
cp "$CONDUCTOR_BIN" "$RUN_INITRAMFS_DIR/bin/obmm-test-conductor"
cp "$ORCHESTRATOR_BIN" "$RUN_INITRAMFS_DIR/bin/obmm-test-orchestrator"
chmod +x "$RUN_INITRAMFS_DIR/bin/obmm-test-conductor" "$RUN_INITRAMFS_DIR/bin/obmm-test-orchestrator"
(
  cd "$RUN_INITRAMFS_DIR"
  find . -print | cpio -o -H newc --quiet | gzip -9 > "$RUN_INITRAMFS"
)
rm -rf "$RUN_INITRAMFS_DIR"

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

cleanup() {
  set +e
  local pid_file
  for pid_file in "$NODEA_PID_FILE" "$NODEB_PID_FILE"; do
    if [[ -f "$pid_file" ]]; then
      local pid=$(cat "$pid_file" 2>/dev/null || true)
      if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null
        sleep 1
        kill -9 "$pid" 2>/dev/null
      fi
      rm -f "$pid_file"
    fi
  done
  # Belt and braces: kill any straggler using this run's shared fabric dir.
  pkill -9 -f "UB_FM_SHARED_DIR=$SHARED_DIR" 2>/dev/null
  pkill -9 -f "ub-qemu-links-obmm-test" 2>/dev/null
  sleep 0.5
  rm -rf "$SHARED_DIR"
  set -e
}

kill_stale_obmm_test_vms() {
  set +e
  local pid_file pid
  for pid_file in "$OUT_DIR"/ub_nodeA.obmm_test.*.pid "$OUT_DIR"/ub_nodeB.obmm_test.*.pid; do
    [[ -f "$pid_file" ]] || continue
    pid=$(cat "$pid_file" 2>/dev/null)
    if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
      echo "[obmm-test] killing stale vm pid=$pid file=$pid_file" >&2
      kill -9 "$pid" 2>/dev/null
    fi
    rm -f "$pid_file"
  done
  pkill -9 -f "ub-qemu-links-obmm-test" 2>/dev/null
  sleep 1
  set -e
}
trap 'cleanup' EXIT INT TERM
kill_stale_obmm_test_vms

start_node() {
  local node_name="$1"
  local role="$2"
  local node_idx="$3"
  local guest_log="$4"
  local qemu_log="$5"
  local pid_file="$6"
  local qemu_extra=()
  local local_ip

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi

  case "$node_name" in
    nodeA) local_ip="10.0.0.1" ;;
    nodeB) local_ip="10.0.0.2" ;;
    *) return 1 ;;
  esac

  env \
    UB_FM_NODE_ID="$node_name" \
    UB_FM_TOPOLOGY_FILE="$TOPOLOGY_FILE" \
    UB_FM_SHARED_DIR="$SHARED_DIR" \
    UB_SIM_ENTITY_COUNT="$ENTITY_COUNT" \
    UB_FM_ENTITY_PLAN_FILE="$ENTITY_PLAN_FILE" \
    "$QEMU_BIN" \
      -M virt,gic-version=3,its=on,ummu=on,ub-cluster-mode=on \
      -cpu cortex-a57 \
      -m 8G \
      -nodefaults \
      -nographic \
      -serial file:"$guest_log" \
      "${qemu_extra[@]}" \
      -kernel "$KERNEL_IMAGE" \
      -initrd "$RUN_INITRAMFS" \
      -append "console=ttyAMA0 rdinit=/bin/run_app linqu_obmm_test=1 obmm_test_local_ip=${local_ip} obmm_test_all_ips=${OBMM_TEST_ALL_IPS} obmm_test_port=${OBMM_TEST_PORT} obmm_test_group_exclude=${OBMM_TEST_GROUP_EXCLUDE} obmm_test_group_include=${OBMM_TEST_GROUP_INCLUDE} obmm_test_include=${OBMM_TEST_INCLUDE} obmm_test_exclude=${OBMM_TEST_EXCLUDE} obmm_test_timeout_scale=${OBMM_TEST_TIMEOUT_SCALE} linqu_urma_dp_role=${role} linqu_node_idx=${node_idx} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &

  echo $! > "$pid_file"
}

wait_for_completion() {
  local deadline=$((SECONDS + RUN_SECS))
  while (( SECONDS < deadline )); do
    if grep -q '\[obmm_test\] orchestrator done' "$NODEA_GUEST_LOG" 2>/dev/null; then
      sleep 5
      return 0
    fi
    if grep -qE '\[obmm_test\] FAIL|Kernel panic - not syncing|Call trace:' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG" 2>/dev/null; then
      return 1
    fi
    sleep 5
  done
  return 1
}

report_results() {
  set +e
  echo "[obmm-test] ---- orchestrator summary (nodeA serial log) ----"
  grep -E "Passed: |Error: |Timeout: |Skipped: |Wall: " "$NODEA_GUEST_LOG" | tail -5
  echo "[obmm-test] ---- per-test outcomes ----"
  grep -E "outcome=|passed|error|timeout" "$NODEA_GUEST_LOG" | grep -E "\[master\]" | tail -40
  echo "[obmm-test] logs: $LOG_DIR/${RUN_ID}"
  set -e
}

start_node nodeA home 1 "$NODEA_GUEST_LOG" "$NODEA_QEMU_LOG" "$NODEA_PID_FILE"
start_node nodeB peer 2 "$NODEB_GUEST_LOG" "$NODEB_QEMU_LOG" "$NODEB_PID_FILE"

echo "[obmm-test] cluster up run_id=$RUN_ID port=$OBMM_TEST_PORT group_exclude=$OBMM_TEST_GROUP_EXCLUDE timeout_scale=$OBMM_TEST_TIMEOUT_SCALE"

if ! wait_for_completion; then
  echo "[obmm-test] FAIL: run did not complete or hit a fatal marker" >&2
  report_results
  exit 1
fi

report_results

if grep -q '\[obmm_test\] pass' "$NODEA_GUEST_LOG" && \
   ! grep -qE '\[obmm_test\] fail' "$NODEA_GUEST_LOG" "$NODEB_GUEST_LOG"; then
  echo "[obmm-test] PASS: framework run completed"
  exit 0
fi
echo "[obmm-test] FAIL: check serial logs" >&2
exit 1
