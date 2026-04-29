#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/four_node_w4_guest.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w4guest4_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless4"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
DEMO_WAIT_SECS="${DEMO_WAIT_SECS:-300}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((54100 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/private/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense_0_6b}"

NODE_IDS=(nodeA nodeB nodeC nodeD)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4)
ALL_IPS_CSV="${(j:,:)NODE_IPS}"

trace() {
  local msg="$1"
  printf '[w4guest4] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
}

wait_for_log_pattern() {
  local file="$1"
  local pattern="$2"
  local timeout_s="$3"
  local deadline=$((SECONDS + timeout_s))
  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]] && rg -q "$pattern" "$file"; then
      return 0
    fi
    sleep 0.2
  done
  return 1
}

wait_for_log_pass_or_fail_since() {
  local file="$1"
  local start_line="$2"
  local pass_pattern="$3"
  local fail_pattern="$4"
  local timeout_s="$5"
  local deadline=$((SECONDS + timeout_s))
  local tmp

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$pass_pattern"; then
        return 0
      fi
      if [[ -n "$tmp" ]] && printf '%s\n' "$tmp" | rg -q "$fail_pattern"; then
        return 1
      fi
    fi
    sleep 0.2
  done
  return 2
}

assert_log_has() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if ! rg -q "$pattern" "$file"; then
    echo "missing log marker: $label in $file" >&2
    return 1
  fi
}

assert_log_absent() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if rg -q "$pattern" "$file"; then
    echo "unexpected log marker: $label in $file" >&2
    return 1
  fi
}

node_index() {
  case "$1" in
    nodeA) echo 1 ;;
    nodeB) echo 2 ;;
    nodeC) echo 3 ;;
    nodeD) echo 4 ;;
    *) return 1 ;;
  esac
}

node_ip() {
  local idx="$(node_index "$1")"
  echo "${NODE_IPS[$idx]}"
}

node_serial_port() {
  local node_id="$1"
  local port_base="$2"
  local idx="$(node_index "$node_id")"
  echo $((port_base + 31 + idx))
}

send_serial_block() {
  local port="$1"
  local payload="$2"
  python3 - "$port" "$payload" <<'PY'
import socket
import sys
import time
port = int(sys.argv[1])
payload = sys.argv[2]
deadline = time.time() + 20.0
last_err = None
while time.time() < deadline:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.connect(("127.0.0.1", port))
        s.sendall(payload.encode("utf-8"))
        time.sleep(0.2)
        s.close()
        sys.exit(0)
    except OSError as exc:
        last_err = exc
        try:
            s.close()
        except OSError:
            pass
        time.sleep(0.5)
raise last_err if last_err is not None else TimeoutError("serial connect timeout")
PY
}

wait_for_guest_shell() {
  local file="$1"
  local timeout_s="$2"
  wait_for_log_pattern "$file" "\\[run_demo\\] boot flow completed, dropping to shell" "$timeout_s"
}

cleanup_headless_env() {
  local cleanup_script="$1"
  if [[ -x "$cleanup_script" ]]; then
    "$cleanup_script" >/dev/null 2>&1 || true
  fi
}

send_w4_cmd() {
  local node_id="$1"
  local local_ip="$2"
  local serial_port="$3"
  local start_marker="$4"
  local payload

  payload=$'export LINQU_UB_ROLE='"${node_id}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_ALL_IPS='"${ALL_IPS_CSV}"$'\n'
  payload+=$'export LINQU_UB_NODE_COUNT=4\n'
  payload+=$'export LINQU_W4_DB_CLUSTER=1\n'
  payload+=$'export LINQU_W4_REQUIRE_UAPI_RESOURCE=1\n'
  payload+=$'export SIM_UAPI_W4_CHIPBACKEND_PROFILE='"${SIM_UAPI_W4_CHIPBACKEND_PROFILE}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_w4_guest\n'

  send_serial_block "$serial_port" "$payload"
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local expected_dispatch_word="0x41a0000041a00000"
  local owner1_role="nodeA"
  local owner2_role="nodeB"
  local owner3_role="nodeC"
  local owner4_role="nodeD"
  local idx
  local remote_idx

  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "host_matmul" || "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_0_6b" ]]; then
    expected_dispatch_word="0x3f8000003f800000"
  fi

  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_kvcache_path=ready" "$node_id obmm kvcache backing" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_cluster_mode=resource_backed_uapi" "$node_id db cluster resource-backed mode" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster=resource_backed_assertions_ok nodes=4 peers=3 .* remote_block=block/w4-.*-block-0 .* group_members=2" "$node_id resource-backed db cluster assertions" || return 1
  idx="$(node_index "$node_id")"
  remote_idx=$((idx % 4 + 1))
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=weight_tile key=weights/qwen3-0\\.6b/node${idx}/tile0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm weight publish" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=kvcache_block key=kvcache/w4/node${idx}/block0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm kvcache publish" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=weight_tile key=weights/qwen3-0\\.6b/node${remote_idx}/tile0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote weight resolve" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=kvcache_block key=kvcache/w4/node${remote_idx}/block0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote kvcache resolve" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0=payload_backing_resolved local=node${idx} remote=node${remote_idx} objects=2 bytes=8192 boundary_offsets=0,248,256,4088,4096 backing=obmm_pool metadata=db status=ok" "$node_id obmm payload backing resolved" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=open_resource ok path=" "$node_id uapi resource opened" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_endpoint ok" "$node_id uapi endpoint mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_queues ok" "$node_id uapi queues mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=queue_phys ok" "$node_id uapi queue phys" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=read_default_segment ok segment=[0-9]+" "$node_id uapi default segment" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_seeded segment=[0-9]+ bytes=8192 checksum=0x[0-9a-f]+" "$node_id uapi kvcache payload seeded" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_boundaries segment=[0-9]+ offsets=0,248,256,4088,4096,4104 status=ok" "$node_id uapi kvcache payload boundaries" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=128 puts=1 gets=1 role=hot_shared" "$node_id uapi kvcache shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=8192 puts=1 gets=1 role=multi_block_boundary" "$node_id uapi kvcache boundary shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]*" "$node_id uapi kvcache db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* role=aux_block" "$node_id uapi kvcache aux db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ writes=1 reads=1" "$node_id uapi kvcache block descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-1 segment=[0-9]+ writes=1 reads=1 role=aux_block_boundary" "$node_id uapi kvcache aux block descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_chipbackend_dispatch_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ task_id=31" "$node_id uapi chipbackend descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=doorbell ok slots=15" "$node_id uapi doorbell" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=wait_completions ok cq_tail=15" "$node_id uapi completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=decode_completions ok" "$node_id decode completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_dispatch_result segment=[0-9]+ word0=${expected_dispatch_word}" "$node_id dispatch payload result" || return 1
  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_0_6b" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_service_flow object=partial_result_tile publish=8 resolve_remote=8 round1_compute=8 storage=block metadata=db status=ok" "$node_id qwen3 service flow" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] completion_sources chipbackend=[1-9][0-9]* shmem=[2-9][0-9]* dfs=[2-9][0-9]* db=[2-9][0-9]* block=[2-9][0-9]* guest_uapi=[0-9]+" "$node_id completion source coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] completion_status success=15 retryable=0 fatal=0" "$node_id completion status" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared" "$node_id uapi kvcache shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=8192 puts=1 gets=1 source=shmem_service role=multi_block_boundary" "$node_id uapi kvcache boundary shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-0 writes=1 reads=1 source=block_service" "$node_id uapi kvcache block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_completion block=w4-${node_id}-block-1 writes=1 reads=1 source=block_service role=aux_block_boundary" "$node_id uapi kvcache aux block completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service" "$node_id uapi kvcache db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_completion key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* puts=1 gets=1 source=db_service role=aux_block" "$node_id uapi kvcache aux db completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] assessment service_coverage=5/5 dispatch_path=ubc_entity_chipbackend kvcache_shmem_segment=[0-9]+ kvcache_block=w4-${node_id}-block-0 kvcache_db_key=block/w4-${node_id}-block-0 kvcache_db_bytes=[1-9][0-9]* complete=true" "$node_id service coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] dispatch path=ubc_entity_chipbackend" "$node_id chipbackend dispatch marker" || return 1
  assert_log_absent "$log_file" "observer_metadata_only" "$node_id no observer-only path" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] pass" "$node_id pass" || return 1
  assert_log_absent "$log_file" "\\[w4_guest\\] fail" "$node_id fail" || return 1
  return 0
}

run_w4_demo() {
  local node_id guest_log start_line serial_port local_ip rc
  typeset -A START_LINES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_line="$(wc -l < "$guest_log" 2>/dev/null || echo 0)"
    START_LINES[$node_id]="$start_line"
    serial_port="$(node_serial_port "$node_id" "$PORT_BASE")"
    local_ip="$(node_ip "$node_id")"
    trace "start w4 guest on $node_id serial=$serial_port local_ip=$local_ip"
    send_w4_cmd "$node_id" "$local_ip" "$serial_port" "[w4guest4] start ${node_id}"
  done

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "\\[w4_guest\\] pass" "\\[w4_guest\\] fail" "$DEMO_WAIT_SECS" || rc=$?
    rc="${rc:-0}"
    if [[ "$rc" != "0" ]]; then
      trace "FAIL: w4 guest did not pass on $node_id rc=$rc"
      return 1
    fi
    validate_node_log "$node_id" "$guest_log" || return 1
  done
  return 0
}

prepare_environment() {
  local guest_log node_id

  mkdir -p "$RUN_DIR"
  : > "$TRACE_FILE"
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  ENV_FILE="$OUT_DIR/headless_four_node_env.${RUN_ID_BASE}.sh" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" \
    SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
    SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
    "$SCRIPT_DIR/launch_ub_four_node_headless.sh" >/dev/null
  if [[ ! -f "$OUT_DIR/headless_four_node_env.${RUN_ID_BASE}.sh" ]]; then
    trace "FAIL: missing headless env file for run_id=$RUN_ID_BASE"
    return 1
  fi
  source "$OUT_DIR/headless_four_node_env.${RUN_ID_BASE}.sh"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "wait shell gate: $node_id"
    if ! wait_for_guest_shell "$guest_log" "$BOOT_WAIT_SECS"; then
      trace "FAIL: shell gate timeout for $node_id"
      return 1
    fi
  done
  trace "shell gate ok for all four nodes"
  return 0
}

main() {
  local exit_code=1

  if ! prepare_environment; then
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if run_w4_demo; then
    exit_code=0
    trace "PASS: four-node w4 guest resource-backed uapi/chipbackend service coverage validated"
    echo "four-node w4 guest validation passed"
  else
    trace "FAIL: four-node w4 guest resource-backed uapi/chipbackend service coverage validation failed"
  fi

  [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
  exit "$exit_code"
}

main "$@"
