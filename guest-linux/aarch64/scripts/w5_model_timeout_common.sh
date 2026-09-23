#!/bin/zsh

# Resolve the managed control/data operation deadline. Bulk I/O waits only for
# one node's range, while direct callables are dispatched before their upstream
# input arrives and must cover the complete pipeline wait plus local compute.
# Callers may provide an explicit positive timeout; otherwise Qwen keeps the
# existing five-minute budget and DeepSeek derives a bounded model deadline.
w5_resolve_managed_timeout_ms() {
  local managed_enabled="${SIM_W5_MANAGED_SERVING:-0}"
  local configured_timeout="${SIM_W5_MANAGED_TIMEOUT_MS:-}"
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local backend="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}"
  local model_io="${SIM_W5_MODEL_IO:-bulk}"
  local node_count="${SIM_W5_CLUSTER_NODE_COUNT:-8}"
  local token_csv="${SIM_LLM_INFER_PROMPT_TOKEN_IDS:-}"
  local max_timeout_ms=86400000
  local total_layers max_layers layer_wait_secs overhead_secs timeout_secs
  local -a prompt_tokens

  case "$managed_enabled" in
    1|true|TRUE|yes|YES)
      ;;
    *)
      return 0
      ;;
  esac

  if [[ -n "$configured_timeout" ]]; then
    if [[ ! "$configured_timeout" =~ '^[1-9][0-9]*$' ]] ||
       (( configured_timeout > max_timeout_ms )); then
      echo "SIM_W5_MANAGED_TIMEOUT_MS must be 1..$max_timeout_ms: $configured_timeout" >&2
      return 2
    fi
    export SIM_W5_MANAGED_TIMEOUT_MS="$configured_timeout"
    return 0
  fi

  case "$profile" in
    deepseek_v4_flash_decode)
      case "$backend" in
        deepseek-v4-flash-simpler|deepseek_v4_flash_simpler)
          total_layers=43
          layer_wait_secs=30
          overhead_secs=300
          ;;
        *)
          export SIM_W5_MANAGED_TIMEOUT_MS=300000
          return 0
          ;;
      esac
      case "$node_count" in
        2|3|4|8)
          ;;
        *)
          echo "cannot derive managed timeout for node count: $node_count" >&2
          return 2
          ;;
      esac
      # Raw prompts are tokenized by the runtime after config loading. Leave the
      # value unresolved here so the runtime can call this function again.
      if [[ -z "$token_csv" ]]; then
        return 0
      fi
      if [[ ! "$token_csv" =~ '^[0-9]+(,[0-9]+)*$' ]]; then
        echo "cannot derive managed timeout from prompt token IDs: $token_csv" >&2
        return 2
      fi
      prompt_tokens=("${(@s:,:)token_csv}")
      if [[ "$model_io" == "direct" ]]; then
        max_layers="$total_layers"
      else
        max_layers=$(( (total_layers + node_count - 1) / node_count ))
      fi
      timeout_secs=$(( ${#prompt_tokens[@]} * max_layers * layer_wait_secs + overhead_secs ))
      export SIM_W5_MANAGED_TIMEOUT_MS="$(( timeout_secs * 1000 ))"
      ;;
    *)
      export SIM_W5_MANAGED_TIMEOUT_MS=300000
      ;;
  esac
}

# Direct model kernels wait for a PTO completion that covers the same largest
# per-node range as the managed data-plane operation. Preserve an explicit PTO
# timeout; otherwise inherit the already-resolved managed range deadline.
w5_resolve_direct_pto_timeout_ms() {
  local model_io="${SIM_W5_MODEL_IO:-bulk}"
  local configured_timeout="${SIM_W5_PTO_UB_GM_TIMEOUT_MS:-}"
  local managed_timeout="${SIM_W5_MANAGED_TIMEOUT_MS:-}"
  local max_timeout_ms=86400000

  if [[ "$model_io" != "direct" ]]; then
    return 0
  fi

  if [[ -n "$configured_timeout" ]]; then
    if [[ ! "$configured_timeout" =~ '^[1-9][0-9]*$' ]] ||
       (( configured_timeout > max_timeout_ms )); then
      echo "SIM_W5_PTO_UB_GM_TIMEOUT_MS must be 1..$max_timeout_ms: $configured_timeout" >&2
      return 2
    fi
    export SIM_W5_PTO_UB_GM_TIMEOUT_MS="$configured_timeout"
    return 0
  fi

  # Raw prompts are tokenized by the runtime. Its second resolution pass fills
  # both deadlines once SIM_LLM_INFER_PROMPT_TOKEN_IDS is available.
  if [[ -z "$managed_timeout" ]]; then
    return 0
  fi
  export SIM_W5_PTO_UB_GM_TIMEOUT_MS="$managed_timeout"
}

# Simpler latches its scheduler watchdog when the callable device is loaded.
# Direct model ranges can legitimately run longer than its generic default, so
# keep that internal watchdog within the outer PTO completion deadline.
w5_resolve_direct_scheduler_timeout_ms() {
  local model_io="${SIM_W5_MODEL_IO:-bulk}"
  local configured_timeout="${SIMPLER_SCHEDULER_TIMEOUT_MS:-}"
  local pto_timeout="${SIM_W5_PTO_UB_GM_TIMEOUT_MS:-}"
  local max_timeout_ms=2147483647

  if [[ "$model_io" != "direct" ]]; then
    return 0
  fi

  if [[ -n "$configured_timeout" ]]; then
    if [[ ! "$configured_timeout" =~ '^[1-9][0-9]*$' ]] ||
       (( configured_timeout > max_timeout_ms )); then
      echo "SIMPLER_SCHEDULER_TIMEOUT_MS must be 1..$max_timeout_ms: $configured_timeout" >&2
      return 2
    fi
    if [[ -n "$pto_timeout" ]] && (( configured_timeout > pto_timeout )); then
      echo "SIMPLER_SCHEDULER_TIMEOUT_MS must not exceed SIM_W5_PTO_UB_GM_TIMEOUT_MS" >&2
      return 2
    fi
    export SIMPLER_SCHEDULER_TIMEOUT_MS="$configured_timeout"
    return 0
  fi

  if [[ -z "$pto_timeout" ]]; then
    return 0
  fi
  export SIMPLER_SCHEDULER_TIMEOUT_MS="$pto_timeout"
}

# Resolve how long the control plane tolerates a provider refresh gap. QEMU can
# prevent the guest provider worker from being scheduled while a long simpler
# range executes. The DeepSeek profile therefore uses its resolved range
# deadline; shorter Qwen ranges keep the existing two-minute fail-closed window.
w5_resolve_managed_provider_lease_ms() {
  local managed_enabled="${SIM_W5_MANAGED_SERVING:-0}"
  local configured_lease="${SIM_W5_MANAGED_PROVIDER_LEASE_MS:-}"
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local backend="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}"
  local min_lease_ms=100
  local max_lease_ms=86400000
  local range_timeout_ms

  case "$managed_enabled" in
    1|true|TRUE|yes|YES)
      ;;
    *)
      return 0
      ;;
  esac

  if [[ -n "$configured_lease" ]]; then
    if [[ ! "$configured_lease" =~ '^[1-9][0-9]*$' ]] ||
       (( configured_lease < min_lease_ms || configured_lease > max_lease_ms )); then
      echo "SIM_W5_MANAGED_PROVIDER_LEASE_MS must be $min_lease_ms..$max_lease_ms: $configured_lease" >&2
      return 2
    fi
    export SIM_W5_MANAGED_PROVIDER_LEASE_MS="$configured_lease"
    return 0
  fi

  case "$profile:$backend" in
    deepseek_v4_flash_decode:deepseek-v4-flash-simpler|deepseek_v4_flash_decode:deepseek_v4_flash_simpler)
      range_timeout_ms="${SIM_W5_PTO_UB_GM_TIMEOUT_MS:-${SIM_W5_MANAGED_TIMEOUT_MS:-}}"
      if [[ ! "$range_timeout_ms" =~ '^[1-9][0-9]*$' ]] ||
         (( range_timeout_ms > max_lease_ms )); then
        echo "cannot derive provider lease from range timeout: $range_timeout_ms" >&2
        return 2
      fi
      export SIM_W5_MANAGED_PROVIDER_LEASE_MS="$range_timeout_ms"
      ;;
    *)
      export SIM_W5_MANAGED_PROVIDER_LEASE_MS=120000
      ;;
  esac
}
