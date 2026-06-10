#!/bin/zsh

ub_gsva_trace_require() {
  local tag="$1"
  local log_file="$2"
  local pattern="$3"
  local description="$4"

  if [[ ! -f "$log_file" ]]; then
    echo "${tag} FAIL: missing log for ${description}: ${log_file}" >&2
    return 1
  fi
  if ! grep -qE "$pattern" "$log_file"; then
    echo "${tag} FAIL: missing ${description} in ${log_file}" >&2
    return 1
  fi
  return 0
}

ub_gsva_trace_reject() {
  local tag="$1"
  local log_file="$2"
  local pattern="$3"
  local description="$4"

  if [[ ! -f "$log_file" ]]; then
    echo "${tag} FAIL: missing log for ${description}: ${log_file}" >&2
    return 1
  fi
  if grep -qE "$pattern" "$log_file"; then
    echo "${tag} FAIL: unexpected ${description} in ${log_file}" >&2
    return 1
  fi
  return 0
}

validate_ub_gsva_trace_logs() {
  local tag="$1"
  local kind="$2"
  local node_name="$3"
  local qemu_log="$4"
  local guest_log="$5"
  local device_prefix
  local device_name
  local device_cna
  local device_cna_dec
  local rc=0

  case "$kind" in
    npu)
      device_prefix="UB_NPU"
      device_name="NPU"
      ;;
    ssd)
      device_prefix="UB_SSD"
      device_name="SSD"
      ;;
    *)
      echo "${tag} FAIL: unknown GSVA trace kind: ${kind}" >&2
      return 1
      ;;
  esac

  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}: created" \
    "${device_name} device creation on ${node_name}" || rc=1
  device_cna=$(sed -nE "s/.*${device_prefix}: realized cna=(0x[0-9a-fA-F]+).*/\\1/p" "$qemu_log" | head -n 1)
  if [[ -z "$device_cna" ]]; then
    echo "${tag} FAIL: missing ${device_name} realized CNA in ${qemu_log}" >&2
    rc=1
  else
    device_cna_dec=$(( device_cna ))
    ub_gsva_trace_require "$tag" "$qemu_log" \
      "UB_DEV_GSVA: (ReadAcquire|WriteAcquire|read ok|write ok).*cna=${device_cna}([^0-9a-fA-F]|$)" \
      "${device_name} GSVA data path uses device CNA on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$qemu_log" \
      "GSVA_COH: (ReadAcquire|WriteAcquire).*cna=${device_cna_dec}([^0-9]|$)" \
      "${device_name} coherence transition uses device CNA on ${node_name}" || rc=1
  fi
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CMD:" \
    "${device_name} command trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CPL:" \
    "${device_name} completion trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "UB_DEV_GSVA: (ReadAcquire|WriteAcquire|read ok|write ok)" \
    "device GSVA data path on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "GSVA_COH:" \
    "GSVA coherence trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "GSVA_TLB:" \
    "GSVA TLB trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "GSVA_ROUTE: token revoke pending" \
    "token revoke pending trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "GSVA_ROUTE: token revoke ack" \
    "token revoke ack trace on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "GSVA_RETIRE:" \
    "GSVA retire trace on ${node_name}" || rc=1
  ub_gsva_trace_reject "$tag" "$qemu_log" "GVA_TCG_TRANSLATE" \
    "GVA TCG translation fallback on ${node_name}" || rc=1
  ub_gsva_trace_reject "$tag" "$qemu_log" "(direct host pointer|HOST_PTR|host pointer bypass)" \
    "direct host pointer bypass marker on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$guest_log" "TEST: bad token rejection" \
    "guest bad-token test on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$guest_log" "PASS: rejected with TOKEN_DENIED" \
    "guest token denial result on ${node_name}" || rc=1
  ub_gsva_trace_reject "$tag" "$guest_log" "FAIL: expected TOKEN_DENIED" \
    "success completion after token denial on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CPL: .*token_denied=[1-9][0-9]*" \
    "${device_name} token_denied stat on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$guest_log" "TEST: retired segment rejection" \
    "guest retired segment test on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$guest_log" "PASS: rejected with SEGMENT_RETIRED" \
    "guest retired segment result on ${node_name}" || rc=1
  ub_gsva_trace_reject "$tag" "$guest_log" "FAIL: expected SEGMENT_RETIRED" \
    "success completion after retired segment rejection on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CPL: .*stale_epoch=[1-9][0-9]*" \
    "${device_name} stale_epoch stat on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CPL: .*retired_segment=[1-9][0-9]*" \
    "${device_name} retired_segment stat on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$guest_log" "TEST: coherence timeout injection" \
    "guest coherence timeout injection test on ${node_name}" || rc=1
  ub_gsva_trace_require "$tag" "$qemu_log" "${device_prefix}_CPL: .*coh_timeout=[1-9][0-9]*" \
    "${device_name} coh_timeout stat on ${node_name}" || rc=1

  if [[ "$kind" == "ssd" ]]; then
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: injected COH_TIMEOUT without committed block" \
      "guest SSD coherence timeout injection result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: checksum mismatch rejection" \
      "guest checksum mismatch test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: checksum mismatch rejected without output write" \
      "guest checksum mismatch result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$qemu_log" "UB_SSD_CPL: .*checksum_error=[1-9][0-9]*" \
      "SSD checksum_error stat on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$qemu_log" "UB_SSD_CPL: .*version_conflict=[1-9][0-9]*" \
      "SSD version_conflict stat on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: missing block read rejection" \
      "guest SSD missing block read test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: missing block rejected without synthetic payload" \
      "guest SSD missing block read result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: partial BLOCK_READ range offset=" \
      "guest SSD partial BLOCK_READ range result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: FLUSH and STAT command completion" \
      "guest SSD FLUSH/STAT test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: FLUSH/STAT completed backend_profile=memory" \
      "guest SSD FLUSH/STAT result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "LINGQU_BLOCK_WRITE .*status=ok" \
      "guest Lingqu Block write evidence on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "LINGQU_BLOCK_READ .*status=ok" \
      "guest Lingqu Block read evidence on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "LINGQU_DFS_MANIFEST .*path=/lingqu/block/objects/.*status=ok" \
      "guest Lingqu DFS manifest evidence on ${node_name}" || rc=1
  fi

  if [[ "$kind" == "npu" ]]; then
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: injected COH_TIMEOUT without output write" \
      "guest NPU coherence timeout injection result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: NPU NOOP control path" \
      "guest NPU NOOP test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: NOOP completed without data movement" \
      "guest NPU NOOP result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: MEMCOPY rejects extra descriptor" \
      "guest NPU extra descriptor rejection test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: extra descriptor rejected without output write" \
      "guest NPU extra descriptor rejection result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: MEMCOPY size mismatch requires ALLOW_TRUNCATE" \
      "guest NPU truncate rule test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "PASS: mismatch rejected and ALLOW_TRUNCATE copies" \
      "guest NPU truncate rule result on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "TEST: NPU output publish as Block ref and DFS manifest" \
      "guest NPU output publish test on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "LINGQU_BLOCK_WRITE .*payload_kind=npu-output .*status=ok" \
      "guest NPU Lingqu Block output evidence on ${node_name}" || rc=1
    ub_gsva_trace_require "$tag" "$guest_log" "LINGQU_DFS_MANIFEST .*path=/lingqu/npu/execution-artifacts/.*status=ok" \
      "guest NPU Lingqu DFS manifest evidence on ${node_name}" || rc=1
  fi

  return $rc
}

validate_ub_gsva_peer_matrix() {
  local tag="$1"
  local node_name="$2"
  local guest_log="$3"
  local local_idx="$4"
  local node_count="$5"
  local expected_peers=$((node_count - 1))
  local seen_peers=0
  local peer_idx
  local rc=0

  if [[ ! -f "$guest_log" ]]; then
    echo "${tag} FAIL: missing guest log for peer matrix on ${node_name}: ${guest_log}" >&2
    return 1
  fi

  seen_peers="$(grep -c "Testing peer " "$guest_log" 2>/dev/null || true)"
  if (( seen_peers != expected_peers )); then
    echo "${tag} FAIL: ${node_name} peer matrix count=${seen_peers}, want=${expected_peers}" >&2
    rc=1
  fi

  for ((peer_idx = 0; peer_idx < node_count; peer_idx++)); do
    if (( peer_idx == local_idx )); then
      continue
    fi
    ub_gsva_trace_require "$tag" "$guest_log" \
      "Testing peer [0-9]+/[0-9]+ node_idx=${peer_idx}[[:space:]]" \
      "peer ${peer_idx} matrix entry on ${node_name}" || rc=1
  done

  return $rc
}
