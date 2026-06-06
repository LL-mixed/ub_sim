#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_gva_direct_matrix_${RANDOM}}"
LOG_PREFIX="[gva-direct-matrix]"

DEFAULT_MODES=(
  write-read
  sync
  unmap-fault
  dump
  invalid-cache
  overlap
  route-overlap
  invalid-ptag
  invalid-dcna
  token-mismatch
  invalid-upi
  read-cache-write-fault
  write-back-no-sync
  write-back-sync
  mrsw-read-share
  mrsw-conflict
  mrsw-writer-conflict
)

DEFAULT_TCG_MODES=(
  write-read
  unmap-fault
)

split_modes() {
  local raw="$1"
  local -a out=()
  local item

  raw="${raw//,/ }"
  for item in ${(z)raw}; do
    if [[ -n "$item" ]]; then
      out+=("$item")
    fi
  done
  print -r -- "${out[@]}"
}

run_mode() {
  local mode="$1"
  local tcg="$2"
  local suffix="${mode//[^A-Za-z0-9_]/_}"
  local run_id="${RUN_ID_BASE}_${suffix}_tcg${tcg}"

  echo "$LOG_PREFIX mode=$mode sim_gva_tcg=$tcg run_id=$run_id"
  env \
    RUN_ID="$run_id" \
    GVA_DIRECT_MODE="$mode" \
    SIM_GVA_TCG="$tcg" \
    "$SCRIPT_DIR/run_ub_dual_node_gva_direct_test.sh"
}

main() {
  local -a modes=()
  local -a tcg_modes=()
  local mode

  if [[ -n "${GVA_DIRECT_MATRIX_MODES:-}" ]]; then
    modes=($(split_modes "$GVA_DIRECT_MATRIX_MODES"))
  else
    modes=("${DEFAULT_MODES[@]}")
  fi

  if [[ "${GVA_DIRECT_MATRIX_SKIP_TCG:-0}" == "1" ]]; then
    tcg_modes=()
  elif [[ -n "${GVA_DIRECT_MATRIX_TCG_MODES:-}" ]]; then
    tcg_modes=($(split_modes "$GVA_DIRECT_MATRIX_TCG_MODES"))
  else
    tcg_modes=("${DEFAULT_TCG_MODES[@]}")
  fi

  echo "$LOG_PREFIX run_id_base=$RUN_ID_BASE modes=${#modes[@]} tcg_modes=${#tcg_modes[@]}"
  for mode in "${modes[@]}"; do
    run_mode "$mode" 0
  done
  for mode in "${tcg_modes[@]}"; do
    run_mode "$mode" 1
  done
  echo "$LOG_PREFIX PASS"
}

main "$@"
