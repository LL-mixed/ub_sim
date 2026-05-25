w5_resolve_memory_reuse_config() {
  local default_reuse_out_dir="$1"
  local profile="$2"
  local reuse_run_id="${SIM_W5_MEMORY_REUSE_RUN_ID:-}"
  if [[ -z "$reuse_run_id" ]]; then
    return 0
  fi
  if [[ -n "${SIM_W5_MEMORY_DECISION_STORE:-}" ||
        -n "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
    echo "SIM_W5_MEMORY_REUSE_RUN_ID cannot be combined with explicit Memory Service reuse stores or selectors" >&2
    return 2
  fi

  local reuse_out_dir="${SIM_W5_MEMORY_REUSE_OUT_DIR:-$default_reuse_out_dir}"
  local decision_store=""
  local selected_run_id="$reuse_run_id"

  if [[ "$reuse_run_id" == "latest" ]]; then
    local -a candidates
    candidates=("$reuse_out_dir"/w5_memory_runtime_boundary_lookup.*_w5_${profile}_*.json(N.om[1]))
    if (( ${#candidates[@]} == 0 )); then
      echo "SIM_W5_MEMORY_REUSE_RUN_ID=latest found no decision store for profile=$profile in $reuse_out_dir" >&2
      return 2
    fi
    decision_store="${candidates[1]}"
    local base="${decision_store:t}"
    selected_run_id="${base#w5_memory_runtime_boundary_lookup.}"
    selected_run_id="${selected_run_id%.json}"
  else
    if [[ ! "$reuse_run_id" =~ '^[A-Za-z0-9._-]+$' ]]; then
      echo "SIM_W5_MEMORY_REUSE_RUN_ID must be latest or a run id without path separators: $reuse_run_id" >&2
      return 2
    fi
    decision_store="$reuse_out_dir/w5_memory_runtime_boundary_lookup.$selected_run_id.json"
  fi

  local object_store="$reuse_out_dir/w5_object_service_store.$selected_run_id.json"
  if [[ ! -f "$decision_store" ]]; then
    echo "W5 Memory Service reuse decision store is missing: $decision_store" >&2
    return 2
  fi
  if [[ ! -f "$object_store" ]]; then
    echo "W5 Memory Service reuse object store is missing: $object_store" >&2
    return 2
  fi

  export SIM_W5_MEMORY_DECISION_STORE="$decision_store"
  export SIM_W5_MEMORY_DECISION_OBJECT_STORE="$object_store"
  export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID="$selected_run_id"
  export SIM_W5_MEMORY_SHORTPATH_EXECUTE="${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-1}"
}
