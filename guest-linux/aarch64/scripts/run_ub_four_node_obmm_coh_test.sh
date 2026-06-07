#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/../.." && pwd)"
KERNEL_IMAGE="${KERNEL_IMAGE:-$ROOT_DIR/out/Image}"
INITRAMFS_IMAGE="${INITRAMFS_IMAGE:-$ROOT_DIR/out/initramfs.cpio.gz}"
TOPOLOGY_FILE="${TOPOLOGY_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_four_node_full_mesh_one_entity.ini}"
ENTITY_PLAN_FILE="${UB_FM_ENTITY_PLAN_FILE:-$WORKSPACE_ROOT/vendor/ub_topology_one_entity.ini}"
ENTITY_COUNT="${UB_SIM_ENTITY_COUNT:-1}"
SHARED_DIR="${UB_FM_SHARED_DIR:-$ROOT_DIR/out/coh4_links_${RANDOM}}"
LINK_WAIT_SECS="${LINK_WAIT_SECS:-60}"
RUN_SECS="${RUN_SECS:-240}"
QEMU_KEEP_ALIVE_ON_POWEROFF="${QEMU_KEEP_ALIVE_ON_POWEROFF:-0}"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
RUN_ID="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_coh4_${RANDOM}}"

COH_TEST_MODE="${COH_TEST_MODE:-multi_reader}"
COH_TEST_SIZE="${COH_TEST_SIZE:-2097152}"
COH_TEST_ITERS="${COH_TEST_ITERS:-2}"
COH_TEST_TOKEN_VALUE="${COH_TEST_TOKEN_VALUE:-0}"
COH_TEST_GENERATION="${COH_TEST_GENERATION:-1}"
COH_TEST_VERBOSE="${COH_TEST_VERBOSE:-1}"
COH_REQUIRE_LOGS="${COH_REQUIRE_LOGS:-1}"
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"

NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_ROLES=(nodeA nodeB nodeC nodeD)
COH_NODE_IDS=(0 1 2 3)

while [[ $# -gt 0 ]]; do
  case "$1" in
    --mode)
      COH_TEST_MODE="$2"
      shift 2
      ;;
    --size)
      COH_TEST_SIZE="$2"
      shift 2
      ;;
    --iterations)
      COH_TEST_ITERS="$2"
      shift 2
      ;;
    --generation)
      COH_TEST_GENERATION="$2"
      shift 2
      ;;
    --keep)
      QEMU_KEEP_ALIVE_ON_POWEROFF=1
      shift
      ;;
    --help)
      echo "Usage: $0 [--mode <multi_reader|writer_inv|dirty_remote_write>] [--size <bytes>] [--iterations <n>] [--generation <n>] [--keep]"
      exit 0
      ;;
    *)
      echo "[coh4] unknown option: $1" >&2
      exit 1
      ;;
  esac
done

source "$SCRIPT_DIR/qemu_ub_common.sh"
APPEND_EXTRA="$(ensure_sim_kernel_append_defaults "$APPEND_EXTRA")"
QEMU_BIN="$(ensure_qemu_ub_binary "$WORKSPACE_ROOT")"
ensure_ub_guest_artifacts "$ROOT_DIR" "$KERNEL_IMAGE" "$INITRAMFS_IMAGE"

if [[ "$APPEND_EXTRA" != *"pmd_mapping="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} pmd_mapping=25%"
fi
if [[ "$APPEND_EXTRA" != *"obmm.mempool_size="* ]]; then
  APPEND_EXTRA="${APPEND_EXTRA} obmm.mempool_size=512M"
fi

mkdir -p "$LOG_DIR/${RUN_ID}" "$OUT_DIR" "$SHARED_DIR"

cleanup() {
  local node_id pid_file pid
  for node_id in "${NODE_IDS[@]}"; do
    pid_file="$OUT_DIR/ub_${node_id}.coh4.${RUN_ID}.pid"
    if [[ -f "$pid_file" ]]; then
      pid="$(cat "$pid_file" 2>/dev/null || true)"
      if [[ -n "${pid:-}" ]] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        sleep 0.5
        kill -9 "$pid" 2>/dev/null || true
      fi
      rm -f "$pid_file"
    fi
  done
}
trap 'cleanup' EXIT INT TERM

rm -rf "$SHARED_DIR"
mkdir -p "$SHARED_DIR"

start_node() {
  local node_id="$1"
  local role="$2"
  local coh_node_id="$3"
  local exporter="$4"
  local guest_log="$5"
  local qemu_log="$6"
  local pid_file="$7"
  local qemu_extra=()
  local exporter_arg=""

  if [[ "$QEMU_KEEP_ALIVE_ON_POWEROFF" == "1" ]]; then
    qemu_extra=(-no-shutdown)
  fi
  if [[ "$exporter" == "1" ]]; then
    exporter_arg="obmm_coh_test_exporter=1"
  fi

  env \
    UB_FM_NODE_ID="$node_id" \
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
      -initrd "$INITRAMFS_IMAGE" \
      -append "console=ttyAMA0 rdinit=/bin/run_demo obmm_coh_test linqu_urma_dp_role=${role} obmm_coh_test_mode=${COH_TEST_MODE} obmm_coh_test_size=${COH_TEST_SIZE} obmm_coh_test_iters=${COH_TEST_ITERS} obmm_coh_test_node_id=${coh_node_id} obmm_coh_test_node_count=4 obmm_coh_test_token_value=${COH_TEST_TOKEN_VALUE} obmm_coh_test_generation=${COH_TEST_GENERATION} obmm_coh_test_verbose=${COH_TEST_VERBOSE} ${exporter_arg} ${APPEND_EXTRA}" \
      >"$qemu_log" 2>&1 &
  echo $! > "$pid_file"
}

wait_for_fm_links_ready() {
  local timeout_s="${1:-60}"
  local deadline=$((SECONDS + timeout_s))

  while (( SECONDS < deadline )); do
    local ready=0
    local status_file
    while IFS= read -r status_file; do
      [[ -n "$status_file" ]] || continue
      if rg -q "^state=READY" "$status_file"; then
        ready=$((ready + 1))
      fi
    done < <(find "$SHARED_DIR" -maxdepth 1 -type f -name '*.status')
    if (( ready >= 4 )); then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

validate_coh_logs() {
  local logs=("$LOG_DIR/${RUN_ID}"/node*_qemu.log)

  if [[ "$COH_REQUIRE_LOGS" != "1" ]]; then
    return 0
  fi
  if ! rg -q 'GVA_S3_MAP .*cache_policy=4' "${logs[@]}"; then
    echo "[coh4] FAIL: missing directory-MESI cache_policy=4 map evidence" >&2
    return 1
  fi
  case "$COH_TEST_MODE" in
    multi_reader)
      rg -q 'OBMM_COH_GETS' "${logs[@]}" || {
        echo "[coh4] FAIL: multi_reader missing OBMM_COH_GETS evidence" >&2
        return 1
      }
      rg -q 'OBMM_COH_DOWNGRADE|OBMM_COH_DOWNGRADE_ACK' "${logs[@]}" || {
        echo "[coh4] FAIL: multi_reader missing downgrade evidence" >&2
        return 1
      }
      ;;
    writer_inv)
      rg -q 'OBMM_COH_GETM' "${logs[@]}" || {
        echo "[coh4] FAIL: writer_inv missing OBMM_COH_GETM evidence" >&2
        return 1
      }
      rg -q 'OBMM_COH_INV|OBMM_COH_INV_ACK' "${logs[@]}" || {
        echo "[coh4] FAIL: writer_inv missing invalidation evidence" >&2
        return 1
      }
      rg -q 'OBMM_COH_WB|OBMM_COH_WB_ACK' "${logs[@]}" || {
        echo "[coh4] FAIL: writer_inv missing writeback evidence" >&2
        return 1
      }
      ;;
    dirty_remote_write|mixed_rw)
      rg -q 'OBMM_COH_GETM' "${logs[@]}" || {
        echo "[coh4] FAIL: dirty_remote_write missing OBMM_COH_GETM evidence" >&2
        return 1
      }
      rg -q 'OBMM_COH_INV|OBMM_COH_INV_ACK' "${logs[@]}" || {
        echo "[coh4] FAIL: dirty_remote_write missing invalidation evidence" >&2
        return 1
      }
      rg -q 'OBMM_COH_WB|OBMM_COH_WB_ACK' "${logs[@]}" || {
        echo "[coh4] FAIL: dirty_remote_write missing writeback evidence" >&2
        return 1
      }
      ;;
  esac
}

echo "[coh4] run_id=$RUN_ID mode=$COH_TEST_MODE size=$COH_TEST_SIZE iterations=$COH_TEST_ITERS"
echo "[coh4] starting nodeA exporter and nodeB/nodeC/nodeD importers..."

integer i=1
for node_id in "${NODE_IDS[@]}"; do
  role="${NODE_ROLES[$i]}"
  coh_node_id="${COH_NODE_IDS[$i]}"
  exporter=0
  [[ "$node_id" == "nodeA" ]] && exporter=1
  start_node "$node_id" "$role" "$coh_node_id" "$exporter" \
    "$LOG_DIR/${RUN_ID}/${node_id}_guest.log" \
    "$LOG_DIR/${RUN_ID}/${node_id}_qemu.log" \
    "$OUT_DIR/ub_${node_id}.coh4.${RUN_ID}.pid"
  i=$((i + 1))
done

echo "[coh4] waiting for FM links..."
if ! wait_for_fm_links_ready "$LINK_WAIT_SECS"; then
  echo "[coh4] FAIL: FM links not ready" >&2
  exit 1
fi
echo "[coh4] FM links ready"

echo "[coh4] waiting for test completion (timeout ${RUN_SECS}s)..."
deadline=$((SECONDS + RUN_SECS))
while (( SECONDS < deadline )); do
  pass_count=0
  fail=false
  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$LOG_DIR/${RUN_ID}/${node_id}_guest.log"
    if [[ -f "$guest_log" ]] && rg -q 'obmm_coh_test: PASS' "$guest_log"; then
      pass_count=$((pass_count + 1))
    fi
    if [[ -f "$guest_log" ]] && rg -q 'obmm_coh_test: FAIL|\[run_demo\] linqu_ub_obmm_coh_test failed|Kernel panic - not syncing|Call trace:' "$guest_log"; then
      fail=true
    fi
  done
  if [[ "$fail" == "true" ]]; then
    echo "[coh4] FAIL: guest reported failure" >&2
    exit 1
  fi
  if (( pass_count == 4 )); then
    cleanup
    sleep 0.5
    validate_coh_logs
    echo "[coh4] PASS: all four nodes completed"
    exit 0
  fi
  sleep 1
done

echo "[coh4] FAIL: timeout waiting for completion" >&2
exit 1
