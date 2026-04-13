#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ITERATIONS="${ITERATIONS:-1}"
RUN_SECS="${RUN_SECS:-40}"
START_GAP_SECS="${START_GAP_SECS:-1}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_probe_${RANDOM}}"
# Probe flow must not skip linqu_probe. Keep bizmsg + urma verify enabled
# so the script still provides a meaningful end-to-end health gate.
APPEND_EXTRA="${APPEND_EXTRA:-linqu_probe_load_helper=1 linqu_bizmsg_verify=1 linqu_urma_dp_verify=1}"

APPEND_EXTRA="$APPEND_EXTRA" \
RUN_SECS="$RUN_SECS" \
ITERATIONS="$ITERATIONS" \
START_GAP_SECS="$START_GAP_SECS" \
RUN_ID="$RUN_ID_BASE" \
"$SCRIPT_DIR/run_ub_dual_node_urma_dataplane_workload_test.sh"

for i in $(seq 1 "$ITERATIONS"); do
  nodea_log="$ROOT_DIR/logs/${RUN_ID_BASE}_urma_dp_iter${i}/nodeA_guest.log"
  nodeb_log="$ROOT_DIR/logs/${RUN_ID_BASE}_urma_dp_iter${i}/nodeB_guest.log"
  if [[ ! -f "$nodea_log" ]]; then
    nodea_log="$ROOT_DIR/out/ub_nodeA.urma_dp.${i}.log"
  fi
  if [[ ! -f "$nodeb_log" ]]; then
    nodeb_log="$ROOT_DIR/out/ub_nodeB.urma_dp.${i}.log"
  fi
  for log in "$nodea_log" "$nodeb_log"; do
    if [[ ! -f "$log" ]]; then
      echo "[probe] missing log: $log" >&2
      exit 1
    fi
    if rg -q "\\[init\\] linqu_probe signal=" "$log"; then
      echo "[probe] linqu_probe terminated by signal: $log" >&2
      rg -n "\\[init\\] linqu_probe signal=" "$log" >&2 || true
      exit 1
    fi
    if ! rg -q "\\[init\\] linqu_probe exit=0" "$log"; then
      echo "[probe] linqu_probe did not report exit=0: $log" >&2
      rg -n "\\[init\\] linqu_probe" "$log" >&2 || true
      exit 1
    fi
    if rg -q "assertion-failed:" "$log"; then
      echo "[probe] linqu_probe assertion failure: $log" >&2
      rg -n "assertion-failed:" "$log" >&2 || true
      exit 1
    fi
  done
done

echo "[probe] dual-node probe checks passed (${ITERATIONS} iterations)"
