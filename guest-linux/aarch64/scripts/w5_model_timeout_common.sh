#!/bin/zsh

# Resolve the managed control/data operation deadline from the largest model
# range a single pipeline node can execute. Callers may provide an explicit
# positive timeout; otherwise Qwen keeps the existing five-minute budget and
# DeepSeek derives a bounded budget from its prompt and pipeline width.
w5_resolve_managed_timeout_ms() {
  local managed_enabled="${SIM_W5_MANAGED_SERVING:-0}"
  local configured_timeout="${SIM_W5_MANAGED_TIMEOUT_MS:-}"
  local profile="${SIM_UAPI_W5_PROFILE:-qwen3_0_6b_decode}"
  local backend="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-}"
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
      max_layers=$(( (total_layers + node_count - 1) / node_count ))
      timeout_secs=$(( ${#prompt_tokens[@]} * max_layers * layer_wait_secs + overhead_secs ))
      export SIM_W5_MANAGED_TIMEOUT_MS="$(( timeout_secs * 1000 ))"
      ;;
    *)
      export SIM_W5_MANAGED_TIMEOUT_MS=300000
      ;;
  esac
}
