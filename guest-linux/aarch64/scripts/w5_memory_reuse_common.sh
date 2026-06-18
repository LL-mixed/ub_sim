W5_MEMORY_REUSE_MISSING_REASON=""

w5_memory_reuse_summary_completed() {
  local reuse_out_dir="$1"
  local run_id="$2"
  local expected_steps="$3"
  local store_kind="${4:-runtime_boundary}"
  local summary_path="$reuse_out_dir/eight_node_w5_inference_cluster_summary.$run_id.txt"
  W5_MEMORY_REUSE_MISSING_REASON=""
  if [[ ! "$expected_steps" =~ '^[0-9]+$' || "$expected_steps" == "0" ]]; then
    W5_MEMORY_REUSE_MISSING_REASON="invalid expected step count: $expected_steps"
    return 1
  fi
  if [[ ! -f "$summary_path" ]]; then
    W5_MEMORY_REUSE_MISSING_REASON="missing summary file: $summary_path"
    return 1
  fi
  if ! grep -q "summary: .*passed_nodes=8/8" "$summary_path"; then
    W5_MEMORY_REUSE_MISSING_REASON="missing passed_nodes=8/8 completion evidence"
    return 1
  fi
  local step=0
  local node=1
  if [[ "$store_kind" == "runtime_boundary" ]]; then
    local expected_records=$(( expected_steps * 7 ))
    if ! grep -q "memory_boundary_observation_summary: .*records=$expected_records .*steps=$expected_steps/$expected_steps .*nodes=node1,node2,node3,node4,node5,node6,node7" "$summary_path"; then
      W5_MEMORY_REUSE_MISSING_REASON="missing boundary coverage summary for steps=$expected_steps records=$expected_records"
      return 1
    fi
    while (( step < expected_steps )); do
      node=1
      while (( node <= 7 )); do
        if ! grep -q "memory_boundary_observation: .* step=$step node=node$node .* status=ok" "$summary_path"; then
          W5_MEMORY_REUSE_MISSING_REASON="missing boundary observation for step=$step node=node$node"
          return 1
        fi
        (( node += 1 ))
      done
      (( step += 1 ))
    done
  fi
  local expected_service_hits=$(( expected_steps * 7 ))
  if ! grep -q "memory_service_summary: .*steps=$expected_steps/$expected_steps .*actions=jump-to-terminal .*lookup_hits=$expected_service_hits" "$summary_path"; then
    W5_MEMORY_REUSE_MISSING_REASON="missing executable jump-to-terminal Memory Service summary for steps=$expected_steps"
    return 1
  fi
  step=0
  while (( step < expected_steps )); do
    node=1
    while (( node <= 7 )); do
      if ! grep -q "memory_service_step: step=$step .*nodes=[^ ]*node$node.*actions=jump-to-terminal" "$summary_path"; then
        W5_MEMORY_REUSE_MISSING_REASON="missing executable jump-to-terminal Memory Service step coverage for step=$step node=node$node"
        return 1
      fi
      (( node += 1 ))
    done
    (( step += 1 ))
  done
  return 0
}

w5_resolve_memory_reuse_config() {
  local default_reuse_out_dir="$1"
  local profile="$2"
  local expected_steps="${3:-${SIM_QWEN3_GUEST_DECODE_STEPS:-1}}"
  local reuse_run_id="${SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG:-}"
  local reuse_optional=0
  local reuse_auto=0
  local explicit_reuse_selector=0
  if [[ -z "$reuse_run_id" ]]; then
    if [[ -n "${SIM_W5_MEMORY_DECISION_STORE:-}" ||
          -n "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}" ||
          -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}" ||
          -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" ||
          -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" ||
          -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}" ||
          -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
      return 0
    fi
    reuse_run_id="latest"
    reuse_optional=1
    reuse_auto=1
  else
    explicit_reuse_selector=1
  fi
  if [[ ! "$expected_steps" =~ '^[0-9]+$' || "$expected_steps" == "0" ]]; then
    echo "SIM_QWEN3_GUEST_DECODE_STEPS must be a positive integer for Memory Service reuse: $expected_steps" >&2
    return 2
  fi
  if (( explicit_reuse_selector )) && [[ -n "${SIM_W5_MEMORY_DECISION_STORE:-}" ||
        -n "${SIM_W5_MEMORY_DECISION_OBJECT_STORE:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_ID:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_IDS:-}" ||
        -n "${SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_ID:-}" ||
        -n "${SIM_W5_MEMORY_SHORTPATH_DECISION_IDS:-}" ]]; then
    echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG cannot be combined with explicit Memory Service reuse stores or selectors" >&2
    return 2
  fi

  local reuse_out_dir="${SIM_W5_MEMORY_REUSE_OUT_DIR:-$default_reuse_out_dir}"
  local decision_store=""
  local selected_store_kind=""
  local selected_run_id="$reuse_run_id"
  local object_store=""

  if [[ "$reuse_run_id" == "latest" ]]; then
    local -a candidates runtime_candidates object_candidates
    runtime_candidates=("$reuse_out_dir"/w5_memory_runtime_boundary_lookup.*_w5_${profile}_*.json(N.om))
    object_candidates=("$reuse_out_dir"/w5_memory_object_store.*_w5_${profile}_*.json(N.om))
    candidates=("${runtime_candidates[@]}" "${object_candidates[@]}")
    if (( ${#candidates[@]} == 0 )); then
      if (( reuse_optional )); then
        if (( ! reuse_auto )); then
          echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest found no reusable decision store; continuing without Memory Service reuse" >&2
        fi
        return 0
      fi
      echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest found no decision store for profile=$profile in $reuse_out_dir" >&2
      return 2
    fi
    local candidate=""
    local base=""
    local candidate_store_kind=""
    for candidate in "${candidates[@]}"; do
      base="${candidate:t}"
      selected_run_id="${base#w5_memory_object_store.}"
      candidate_store_kind="object_store"
      if [[ "$selected_run_id" == "$base" ]]; then
        selected_run_id="${base#w5_memory_runtime_boundary_lookup.}"
        candidate_store_kind="runtime_boundary"
      fi
      selected_run_id="${selected_run_id%.json}"
      object_store="$reuse_out_dir/w5_object_service_store.$selected_run_id.json"
      if [[ -f "$object_store" ]] &&
          w5_memory_reuse_summary_completed "$reuse_out_dir" "$selected_run_id" "$expected_steps" "$candidate_store_kind"; then
        decision_store="$candidate"
        selected_store_kind="$candidate_store_kind"
        break
      fi
    done
    if [[ -z "$decision_store" ]]; then
      if (( reuse_optional )); then
        if (( ! reuse_auto )); then
          echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest found no completed reusable run covering steps=$expected_steps; continuing without Memory Service reuse" >&2
        fi
        return 0
      fi
      echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG=latest found no completed reusable run covering steps=$expected_steps for profile=$profile in $reuse_out_dir" >&2
      return 2
    fi
  else
    if [[ ! "$reuse_run_id" =~ '^[A-Za-z0-9._-]+$' ]]; then
      echo "SIM_W5_MEMORY_REUSE_RUN_ID_FOR_DEBUG must be latest or a run id without path separators: $reuse_run_id" >&2
      return 2
    fi
    decision_store="$reuse_out_dir/w5_memory_object_store.$selected_run_id.json"
    selected_store_kind="object_store"
    if [[ ! -f "$decision_store" ]]; then
      decision_store="$reuse_out_dir/w5_memory_runtime_boundary_lookup.$selected_run_id.json"
      selected_store_kind="runtime_boundary"
    fi
  fi

  object_store="$reuse_out_dir/w5_object_service_store.$selected_run_id.json"
  if [[ ! -f "$decision_store" ]]; then
    echo "W5 Memory Service reuse decision store is missing: $decision_store" >&2
    return 2
  fi
  if [[ ! -f "$object_store" ]]; then
    echo "W5 Memory Service reuse object store is missing: $object_store" >&2
    return 2
  fi
  if ! w5_memory_reuse_summary_completed "$reuse_out_dir" "$selected_run_id" "$expected_steps" "$selected_store_kind"; then
    echo "W5 Memory Service reuse summary is missing completion/coverage evidence for run_id=$selected_run_id: $W5_MEMORY_REUSE_MISSING_REASON" >&2
    return 2
  fi

  export SIM_W5_MEMORY_DECISION_STORE="$decision_store"
  export SIM_W5_MEMORY_DECISION_OBJECT_STORE="$object_store"
  export SIM_W5_MEMORY_BOUNDARY_OBSERVATION_RUN_ID="$selected_run_id"
  export SIM_W5_MEMORY_SHORTPATH_EXECUTE="${SIM_W5_MEMORY_SHORTPATH_EXECUTE:-1}"
}
