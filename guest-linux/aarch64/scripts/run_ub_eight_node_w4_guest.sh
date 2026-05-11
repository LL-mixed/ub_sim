#!/bin/zsh
set -euo pipefail
setopt null_glob

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
LOG_DIR="$ROOT_DIR/logs"
TRACE_FILE="${TRACE_FILE:-$OUT_DIR/eight_node_w4_guest.trace.latest.txt}"
RUN_ID_BASE="${RUN_ID:-$(date +%Y-%m-%d_%H-%M-%S)_w4guest8_${RANDOM}}"
RUN_DIR="$LOG_DIR/${RUN_ID_BASE}_headless8"
UB_FM_SHARED_DIR="${UB_FM_SHARED_DIR:-/tmp/ub8-${RANDOM}}"
BOOT_WAIT_SECS="${BOOT_WAIT_SECS:-180}"
DEMO_WAIT_SECS="${DEMO_WAIT_SECS:-600}"
APPEND_BASE="${APPEND_EXTRA:-linqu_probe_skip=1 linqu_probe_load_helper=1}"
PORT_BASE_START="${PORT_BASE_START:-$((56100 + (RANDOM % 300)))}"
PORT_BASE="$PORT_BASE_START"
PORT_NUM="${UB_SIM_PORT_NUM:-7}"
SIMPLER_HOST_MATMUL_MANIFEST="${SIMPLER_HOST_MATMUL_MANIFEST:-/tmp/simpler-host-matmul-artifacts/host_matmul_manifest.json}"
SIM_UAPI_W4_CHIPBACKEND_PROFILE="${SIM_UAPI_W4_CHIPBACKEND_PROFILE:-qwen3_dense_0_6b}"
SIM_QWEN3_GUEST_DECODE_STEPS="${SIM_QWEN3_GUEST_DECODE_STEPS:-1}"
SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS="${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-9707,1207,16948,18}"
SIM_QWEN3_GUEST_ENGRAM="${SIM_QWEN3_GUEST_ENGRAM:-0}"
SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE="${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE:-8}"
SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE="${SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE:-0}"
SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI="${SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI:-1000}"
SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW="${SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW:-0}"
SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS="${SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS:-}"
SIM_W4_UAPI_COMPLETION_TIMEOUT_MS="${SIM_W4_UAPI_COMPLETION_TIMEOUT_MS:-900000}"
SIM_W4_RESOURCE_ASSERTIONS="${SIM_W4_RESOURCE_ASSERTIONS:-0}"
FATAL_GUEST_PATTERN="rcu_preempt|RCU grace-period|self-detected stall|detected stalls on CPUs/tasks|rx msg plen invalid|poller rx msg failed, ret=-22|timeout waiting completions|\\[w4_guest\\] fail"
FATAL_QEMU_PATTERN="SIM_DEC: cpu read failed|ub_link write failed|bounded write timed out|rx msg plen invalid|poller rx msg failed"

NODE_IDS=(nodeA nodeB nodeC nodeD nodeE nodeF nodeG nodeH)
NODE_IPS=(10.0.0.1 10.0.0.2 10.0.0.3 10.0.0.4 10.0.0.5 10.0.0.6 10.0.0.7 10.0.0.8)
ALL_IPS_CSV="${(j:,:)NODE_IPS}"

trace() {
  local msg="$1"
  printf '[w4guest8] %s\n' "$msg" | tee -a "$TRACE_FILE" >&2
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
  local pass_count="${6:-1}"
  local deadline=$((SECONDS + timeout_s))
  local tmp
  local count

  while (( SECONDS < deadline )); do
    if [[ -f "$file" ]]; then
      tmp="$(tail -n "+$((start_line + 1))" "$file" 2>/dev/null || true)"
      count=0
      if [[ -n "$tmp" ]]; then
        count="$(printf '%s\n' "$tmp" | rg -c "$pass_pattern" || true)"
      fi
      if (( count >= pass_count )); then
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

assert_no_fatal_runtime_logs() {
  local node_id guest_log qemu_log

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    qemu_log="$RUN_DIR/${node_id}_qemu.log"
    assert_log_absent "$guest_log" "$FATAL_GUEST_PATTERN" "$node_id fatal guest runtime marker" || return 1
    assert_log_absent "$qemu_log" "$FATAL_QEMU_PATTERN" "$node_id fatal qemu runtime marker" || return 1
  done
  return 0
}

node_index() {
  case "$1" in
    nodeA) echo 1 ;;
    nodeB) echo 2 ;;
    nodeC) echo 3 ;;
    nodeD) echo 4 ;;
    nodeE) echo 5 ;;
    nodeF) echo 6 ;;
    nodeG) echo 7 ;;
    nodeH) echo 8 ;;
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

port_block_available() {
  local port_base="$1"
  python3 - "$port_base" <<'PY'
import socket
import sys

base = int(sys.argv[1])
ports = list(range(base, base + 8)) + list(range(base + 32, base + 40))
sockets = []
try:
    for port in ports:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", port))
        sockets.append(sock)
except OSError:
    sys.exit(1)
finally:
    for sock in sockets:
        sock.close()
PY
}

choose_port_base() {
  local candidate="$PORT_BASE_START"
  local attempt=0
  while (( attempt < 80 )); do
    if port_block_available "$candidate"; then
      PORT_BASE="$candidate"
      return 0
    fi
    candidate=$((candidate + 64))
    attempt=$((attempt + 1))
  done
  echo "failed to find available eight-node port block from base $PORT_BASE_START" >&2
  return 1
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
        for line in payload.splitlines(True):
            s.sendall(line.encode("utf-8"))
            time.sleep(0.05)
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
  local decode_step="$5"
  local payload

  payload=$'export LINQU_UB_ROLE='"${node_id}"$'\n'
  payload+=$'export LINQU_UB_LOCAL_IP='"${local_ip}"$'\n'
  payload+=$'export LINQU_UB_ALL_IPS='"${ALL_IPS_CSV}"$'\n'
  payload+=$'export LINQU_UB_NODE_COUNT=8\n'
  payload+=$'export LINQU_W4_DB_CLUSTER=1\n'
  payload+=$'export LINQU_W4_REQUIRE_UAPI_RESOURCE=1\n'
  payload+=$'export SIM_W4_DB_LAZY_REMOTE_ACTIVATION=1\n'
  payload+=$'export SIM_UAPI_W4_CHIPBACKEND_PROFILE='"${SIM_UAPI_W4_CHIPBACKEND_PROFILE}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_DECODE_STEP='"${decode_step}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_DECODE_STEPS='"${SIM_QWEN3_GUEST_DECODE_STEPS}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS='"${SIM_QWEN3_GUEST_PROMPT_TOKEN_IDS:-}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM='"${SIM_QWEN3_GUEST_ENGRAM}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_SESSION_ID='"${SIM_QWEN3_GUEST_ENGRAM_SESSION_ID:-guest}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE='"${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE='"${SIM_QWEN3_GUEST_ENGRAM_NO_REPEAT_NGRAM_SIZE}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI='"${SIM_QWEN3_GUEST_ENGRAM_REPETITION_PENALTY_MILLI}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW='"${SIM_QWEN3_GUEST_ENGRAM_HISTORY_WINDOW}"$'\n'
  payload+=$'export SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS='"${SIM_QWEN3_GUEST_ENGRAM_BLOCK_TOKEN_IDS}"$'\n'
  payload+=$'export SIM_W4_UAPI_COMPLETION_TIMEOUT_MS='"${SIM_W4_UAPI_COMPLETION_TIMEOUT_MS}"$'\n'
  payload+=$'export SIM_W4_RESOURCE_ASSERTIONS='"${SIM_W4_RESOURCE_ASSERTIONS}"$'\n'
  payload+=$'echo '"${start_marker}"$'\n'
  payload+=$'/bin/linqu_w4_guest\n'

  send_serial_block "$serial_port" "$payload"
}

poweroff_guest_nodes() {
  local node_id serial_port payload

  payload=$'echo 1 >/proc/sys/kernel/sysrq\n'
  payload+=$'echo o >/proc/sysrq-trigger\n'
  for node_id in "${NODE_IDS[@]}"; do
    serial_port="$(node_serial_port "$node_id" "$PORT_BASE")"
    send_serial_block "$serial_port" "$payload"
  done
}

validate_owner_observed() {
  local node_id="$1"
  local log_file="$2"
  local owner_idx="$3"
  local owner_role="$4"

  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=prefix_group key=request/w4-${owner_role}-request-0/prefix-group/${owner_role}-group-0 group=${owner_role}-group-0 members=2 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} group" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=request_prefix key=request/w4-${owner_role}-request-0/prefix/${owner_role}-prefix-0 version=[0-9]+" "$node_id saw ${owner_role} prefix" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-0 state=hot version=[0-9]+" "$node_id saw ${owner_role} block0" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster_observe owner=node${owner_idx} kind=block_meta key=block/w4-${owner_role}-block-1 state=reloaded version=[0-9]+" "$node_id saw ${owner_role} block1" || return 1
}

validate_node_log() {
  local node_id="$1"
  local log_file="$2"
  local expected_dispatch_word="0x41a0000041a00000"

  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "host_matmul" ]]; then
    expected_dispatch_word="0x3f8000003f800000"
  elif [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_0_6b" ]]; then
    expected_dispatch_word="0x[0-9a-f]+"
  fi

  local idx owner_role
  local remote_idx

  assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_kvcache_path=ready" "$node_id obmm kvcache backing" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_cluster_mode=resource_backed_uapi" "$node_id db cluster resource-backed mode" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage db_service_cluster=resource_backed_assertions_(ok|skipped) nodes=8 .*" "$node_id resource-backed db cluster assertions" || return 1
  idx="$(node_index "$node_id")"
  remote_idx=$((idx % 8 + 1))
  if [[ "$SIM_W4_RESOURCE_ASSERTIONS" == "1" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=weight_tile key=weights/qwen3-0\\.6b/node${idx}/tile0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm weight publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=kvcache_block key=kvcache/w4/node${idx}/block0 owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm kvcache publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=hidden_range_input key=hidden/qwen3-0\\.6b/node${idx}/range-input owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range input publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_publish kind=hidden_range_output key=hidden/qwen3-0\\.6b/node${idx}/range-output owner=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm hidden range output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_placement local=node${idx} key=placement/qwen3-0\\.6b/layer-range/node${idx} .* next=node${remote_idx} .* source=db_metadata strategy=balanced_layers status=ok" "$node_id qwen3 range forward placement" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_contract local=node${idx} .* pipeline_nodes=8 total_layers=28 .* balanced=true .*placement_source=db_metadata .*backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_object_desc_put local=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor put" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_object_desc_get remote=node${remote_idx} reader=node${idx} objects=4 queue=obmm_spsc .* status=ok" "$node_id obmm object descriptor get" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=weight_tile key=weights/qwen3-0\\.6b/node${remote_idx}/tile0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote weight resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=kvcache_block key=kvcache/w4/node${remote_idx}/block0 owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote kvcache resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=hidden_range_input key=hidden/qwen3-0\\.6b/node${remote_idx}/range-input owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range input resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0_resolve kind=hidden_range_output key=hidden/qwen3-0\\.6b/node${remote_idx}/range-output owner=node${remote_idx} reader=node${idx} .* backing=obmm_pool metadata=db status=ok" "$node_id obmm remote hidden range output resolve" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_handoff local=node${idx} next=node${remote_idx} .* placement_source=db_metadata backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 range forward handoff" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_summary local=node${idx} nodes=8 layers=28 .* hidden_bytes=[1-9][0-9]* objects=2 .* balanced=true placement_source=db_metadata backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range forward summary" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage obmm_service_v0=payload_backing_resolved local=node${idx} remote=node${remote_idx} objects=4 bytes=8192 hidden_bytes=[1-9][0-9]* boundary_offsets=0,248,256,4088,4096 backing=obmm_pool metadata=db status=ok" "$node_id obmm payload backing resolved" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=open_resource ok path=" "$node_id uapi resource opened" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_endpoint ok" "$node_id uapi endpoint mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=map_queues ok" "$node_id uapi queues mapped" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=queue_phys ok" "$node_id uapi queue phys" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=read_default_segment ok segment=[0-9]+" "$node_id uapi default segment" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_seeded segment=[0-9]+ bytes=8192 checksum=0x[0-9a-f]+" "$node_id uapi kvcache payload seeded" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_boundaries segment=[0-9]+ offsets=0,248,256,4088,4096,4104 status=ok" "$node_id uapi kvcache payload boundaries" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=128 puts=1 gets=1 role=hot_shared" "$node_id uapi kvcache shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_descriptor segment=[0-9]+ bytes=8192 puts=1 gets=1 role=legacy_demo_payload" "$node_id uapi kvcache boundary shmem descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-0 bytes=[1-9][0-9]*" "$node_id uapi kvcache db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_db_descriptor key=block/w4-${node_id}-block-1 bytes=[1-9][0-9]* role=aux_block" "$node_id uapi kvcache aux db descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ writes=1 reads=1" "$node_id uapi kvcache block descriptor" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_block_descriptor block=w4-${node_id}-block-1 segment=[0-9]+ writes=1 reads=1 role=aux_block_boundary" "$node_id uapi kvcache aux block descriptor" || return 1
  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_0_6b" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_dispatch_descriptor node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) segment=[0-9]+ task_id=31 source=db_metadata status=ok" "$node_id qwen3 range dispatch descriptor" || return 1
  else
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_chipbackend_dispatch_descriptor block=w4-${node_id}-block-0 segment=[0-9]+ task_id=31" "$node_id uapi chipbackend descriptor" || return 1
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] step=doorbell ok slots=15" "$node_id uapi doorbell" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=wait_completions ok cq_tail=15" "$node_id uapi completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] step=decode_completions ok" "$node_id decode completions" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_payload_dispatch_result segment=[0-9]+ word0=${expected_dispatch_word}" "$node_id dispatch payload result" || return 1
  if [[ "$SIM_UAPI_W4_CHIPBACKEND_PROFILE" == "qwen3_dense_0_6b" ]]; then
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_compute_contract node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=8 total_layers=28 hidden_bytes=[1-9][0-9]* source=(dispatch_task|runtime_forward) output=(completion|metadata) status=ok" "$node_id qwen3 range compute contract" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_runtime_forward node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ next=$((remote_idx - 1)) pipeline_nodes=8 total_layers=28 hidden_bytes=[1-9][0-9]* input_checksum=0x[0-9a-f]+ output_checksum=0x[0-9a-f]+ range_checksum=0x[0-9a-f]+ real_layers=[0-9]+ payload_offset=0x[0-9a-f]+ payload_bytes=[1-9][0-9]* kv_payload_offset=0x[0-9a-f]+ kv_payload_bytes=[1-9][0-9]* kv_payload_checksum=0x[0-9a-f]+ source=runtime_forward output=metadata status=ok" "$node_id qwen3 range runtime forward" || return 1
    if (( idx > 1 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_runtime_input_loaded node=${idx} layers=\\[[0-9]+,[0-9]+\\) input_offset=0x[0-9a-f]+ input_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* source=obmm_object_service target=uapi_segment status=ok" "$node_id qwen3 runtime range input loaded" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_forward_runtime_output_publish local=node${idx} step=[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ output_checksum=0x[0-9a-f]+ bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 runtime range output publish" || return 1
    assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_kv_state_publish local=node${idx} step=[0-9]+ key=kvcache/qwen3-0\\.6b/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ slot_bytes=[1-9][0-9]* producer_publish_ms=[0-9]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range kv state publish" || return 1
    if (( SIM_QWEN3_GUEST_DECODE_STEPS > 1 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_range_kv_state_resolve local=node${idx} step=[1-9][0-9]* previous_step=[0-9]+ key=kvcache/qwen3-0\\.6b/node${idx}/layers-[0-9]+-[0-9]+/decode-step[0-9]+ layers=\\[[0-9]+,[0-9]+\\) count=[0-9]+ kv_bytes=[1-9][0-9]* kv_checksum=0x[0-9a-f]+ offset=0x[0-9a-f]+ source=object_service backing=obmm_pool metadata=db status=ok" "$node_id qwen3 range kv state resolve" || return 1
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        if (( idx == 1 || idx == 8 || idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_history_wait step=[0-9]+ object_key=qwen3/session/[^/]+/tokens/history owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=[0-9]+ history_tokens=[1-9][0-9]* bytes=[1-9][0-9]* checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram history wait" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_state_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/engram/state owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 history_tokens=[1-9][0-9]* selected_token=[0-9]+ history_checksum=0x[0-9a-f]+ blocked=[0-9]+ fallback=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ bytes=128 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram state wait" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_resolved step=[1-9][0-9]* previous_step=[0-9]+ selected_token=[0-9]+ history_tokens=[1-9][0-9]* history_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ target=next_round_input status=ok" "$node_id qwen3 engram state resolved" || return 1
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_prompt_tokens_from_history tokens=[1-9][0-9]* source=engram_history_object target=uapi_segment status=ok" "$node_id qwen3 prompt tokens from history" || return 1
        else
          assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_decode_round_engram_state_skip step=[1-9][0-9]* local=${node_id} reason=range_worker_stateless status=ok" "$node_id qwen3 engram state skip" || return 1
        fi
      fi
    fi
    if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]] && (( idx == SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_candidates_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk owner=node8 version=1 candidate_count=[1-9][0-9]* bytes=256 checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram candidates wait" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_token_select local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ history_tokens=[1-9][0-9]* raw_token=[0-9]+ runner_up=[0-9]+ selected_token=[0-9]+ candidate_count=[1-9][0-9]* candidate2=[0-9]+ candidate3=[0-9]+ blocked=[0-9]+ fallback=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ no_repeat_ngram_size=[0-9]+ repetition_penalty_milli=[0-9]+ history_window=[0-9]+ candidate_checksum=0x[0-9a-f]+ source=guest_policy status=ok" "$node_id qwen3 engram token select" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_decision_publish local=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} step=[0-9]+ objects=3 history_tokens=[1-9][0-9]* selected_token=[0-9]+ raw_token=[0-9]+ runner_up=[0-9]+ fallback=[0-9]+ blocked=[0-9]+ top_score_milli=-?[0-9]+ runner_up_score_milli=-?[0-9]+ history_window=[0-9]+ history_key=qwen3/session/[^/]+/tokens/history history_version=[0-9]+ selected_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected state_key=qwen3/session/[^/]+/step/[0-9]+/engram/state history_checksum=0x[0-9a-f]+ selected_checksum=0x[0-9a-f]+ state_checksum=0x[0-9a-f]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram decision publish" || return 1
    fi
    if (( idx == 8 )); then
      if [[ "$SIM_QWEN3_GUEST_ENGRAM" == "1" ]]; then
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_candidates_publish local=node8 step=[0-9]+ candidate_count=[1-9][0-9]* candidates_key=qwen3/session/[^/]+/step/[0-9]+/candidates/topk candidates_version=1 candidates_checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 engram candidates publish" || return 1
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_selected_token_wait step=[0-9]+ object_key=qwen3/session/[^/]+/step/[0-9]+/tokens/selected owner=node${SIM_QWEN3_GUEST_ENGRAM_OWNER_NODE} version=1 bytes=64 token=[0-9]+ checksum=0x[0-9a-f]+ source=obmm_object_service status=ok" "$node_id qwen3 engram selected token wait" || return 1
        assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_engram_selected_writeback local=node8 step=[0-9]+ selected_token=[0-9]+ source=engram_selected_object target=terminal_token_result status=ok" "$node_id qwen3 engram selected writeback" || return 1
      fi
      assert_log_has "$log_file" "\\[w4_guest\\] stage qwen3_terminal_token_result_publish local=node8 step=[0-9]+ token=[0-9]+ runner_up=[0-9]+ margin_milli=[0-9]+ logits_checksum=0x[0-9a-f]+ text_checksum=0x[0-9a-f]+ piece_word0=0x[0-9a-f]+ piece_word1=0x[0-9a-f]+ object_key=tokens/qwen3-0\\.6b/decode-step[0-9]+ offset=0x[0-9a-f]+ bytes=64 checksum=0x[0-9a-f]+ epoch=[0-9]+ seq=[0-9]+ backing=obmm_pool metadata=db queue=obmm_spsc status=ok" "$node_id qwen3 terminal token result publish" || return 1
    fi
    assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_range_forward_only object=range_hidden publish=0 resolve_remote=0 compute=0 storage=obmm_object metadata=db status=ok" "$node_id qwen3 range-only flow" || return 1
    if (( idx == 8 )); then
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_logits_sampling_table entries=1 entry_words=45 table_bytes=360 vocab=151936 sampled_distinct=1 logits_checksum_nonzero=1 text_checksum_nonzero=1 real_logits=1 status=ok" "$node_id qwen3 logits sampling table" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_token_text_table entries=1 entry_words=8 table_bytes=64 total_bytes=[1-9][0-9]* piece_bytes=9 policy_kind=[12] policy_hash=0x[0-9a-f]+ packed_matches=1 checksum_matches=1 boundary_first=1 boundary_last=1 status=ok" "$node_id qwen3 token text table" || return 1
    else
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_logits_sampling_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id qwen3 logits sampling table skipped" || return 1
      assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_qwen3_token_text_table node=$((idx - 1)) layers=\\[[0-9]+,[0-9]+\\) terminal_owner=0 status=skipped" "$node_id qwen3 token text table skipped" || return 1
    fi
  fi
  assert_log_has "$log_file" "\\[w4_guest\\] completion_sources chipbackend=[1-9][0-9]* shmem=[2-9][0-9]* dfs=[2-9][0-9]* db=[2-9][0-9]* block=[2-9][0-9]* guest_uapi=[0-9]+" "$node_id completion source coverage" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] completion_status success=15 retryable=0 fatal=0" "$node_id completion status" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=128 puts=1 gets=1 source=shmem_service role=hot_shared" "$node_id uapi kvcache shmem completion" || return 1
  assert_log_has "$log_file" "\\[w4_guest\\] stage uapi_kvcache_shmem_completion segment=[0-9]+ bytes=8192 puts=1 gets=1 source=shmem_service role=legacy_demo_payload" "$node_id uapi kvcache boundary shmem completion" || return 1
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
  local decode_step="$1"
  local node_id guest_log start_line serial_port local_ip rc
  typeset -A START_LINES

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    start_line="$(wc -l < "$guest_log" 2>/dev/null || echo 0)"
    START_LINES[$node_id]="$start_line"
    serial_port="$(node_serial_port "$node_id" "$PORT_BASE")"
    local_ip="$(node_ip "$node_id")"
    trace "start w4 guest step=$decode_step on $node_id serial=$serial_port local_ip=$local_ip"
    send_w4_cmd "$node_id" "$local_ip" "$serial_port" "[w4guest8] start step=${decode_step} ${node_id}" "$decode_step"
  done

  for node_id in "${NODE_IDS[@]}"; do
    rc=0
    guest_log="$RUN_DIR/${node_id}_guest.log"
    wait_for_log_pass_or_fail_since "$guest_log" "${START_LINES[$node_id]}" \
      "^\\[w4_guest\\] pass\\r?$" "$FATAL_GUEST_PATTERN" "$((DEMO_WAIT_SECS * SIM_QWEN3_GUEST_DECODE_STEPS))" \
      "$SIM_QWEN3_GUEST_DECODE_STEPS" || rc=$?
    if [[ "$rc" != "0" ]]; then
      trace "FAIL: w4 guest did not pass on $node_id rc=$rc"
      return 1
    fi
    validate_node_log "$node_id" "$guest_log" || return 1
  done
  assert_no_fatal_runtime_logs || return 1
  return 0
}

prepare_environment() {
  local guest_log node_id

  mkdir -p "$RUN_DIR" "$OUT_DIR"
  : > "$TRACE_FILE"
  choose_port_base
  trace "prepare: launch headless env run_id=$RUN_ID_BASE"
  trace "prepare: port_base=$PORT_BASE"
  ENV_FILE="$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh" PORT_BASE="$PORT_BASE" RUN_ID="$RUN_ID_BASE" APPEND_EXTRA="$APPEND_BASE" UB_SIM_PORT_NUM="$PORT_NUM" \
    UB_FM_SHARED_DIR="$UB_FM_SHARED_DIR" \
    SIMPLER_HOST_MATMUL_MANIFEST="$SIMPLER_HOST_MATMUL_MANIFEST" \
    SIM_UAPI_W4_CHIPBACKEND_PROFILE="$SIM_UAPI_W4_CHIPBACKEND_PROFILE" \
    SIM_QWEN3_0_6B_WEIGHTS_PATH="${SIM_QWEN3_0_6B_WEIGHTS_PATH:-}" \
    "$SCRIPT_DIR/launch_ub_eight_node_headless.sh"
  if [[ ! -f "$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh" ]]; then
    trace "FAIL: headless env file was not created"
    return 1
  fi
  source "$OUT_DIR/headless_eight_node_env.${RUN_ID_BASE}.sh"

  for node_id in "${NODE_IDS[@]}"; do
    guest_log="$RUN_DIR/${node_id}_guest.log"
    trace "wait shell gate: $node_id"
    if ! wait_for_log_pattern "$guest_log" "\\[run_demo\\] boot flow completed, dropping to shell" "$BOOT_WAIT_SECS"; then
      trace "FAIL: shell gate timeout for $node_id"
      return 1
    fi
  done
  trace "shell gate ok for all eight nodes"
  return 0
}

main() {
  local exit_code=1
  local step

  if ! prepare_environment; then
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if ! run_w4_demo 0; then
    trace "FAIL: eight-node w4 guest resource-backed uapi/chipbackend service coverage validation failed"
    [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
    exit 1
  fi

  if true; then
    exit_code=0
    trace "PASS: eight-node w4 guest resource-backed uapi/chipbackend service coverage validated"
    echo "eight-node w4 guest validation passed"
  fi

  poweroff_guest_nodes
  sleep 5
  [[ -n "${CLEANUP_SCRIPT:-}" ]] && cleanup_headless_env "$CLEANUP_SCRIPT"
  exit "$exit_code"
}

main "$@"
