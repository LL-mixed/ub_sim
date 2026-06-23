#!/bin/zsh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT_DIR="$ROOT_DIR/out"
INITRAMFS_DIR="$OUT_DIR/initramfs"
PROBE_SRC="$ROOT_DIR/probe.c"
PROBE_BIN="$OUT_DIR/linqu_probe"
URMA_DP_SRC="$ROOT_DIR/urma_dp.c"
URMA_DP_BIN="$OUT_DIR/linqu_urma_dp"
INIT_SRC="$ROOT_DIR/init.c"
INIT_BIN="$OUT_DIR/init"
INSMOD_SRC="$ROOT_DIR/insmod.c"
INSMOD_BIN="$OUT_DIR/insmod"
INIT_MANUAL_BIND_SRC="$ROOT_DIR/init_manual_bind.c"
INIT_MANUAL_BIND_BIN="$OUT_DIR/init_manual_bind"
CHAT_SRC="$ROOT_DIR/apps/ub_chat/ub_chat.c"
CHAT_BIN="$OUT_DIR/linqu_ub_chat"
RPC_SRC="$ROOT_DIR/apps/ub_rpc/ub_rpc.c"
RPC_BIN="$OUT_DIR/linqu_ub_rpc"
TCP_EACH_SERVER_SRC="$ROOT_DIR/apps/ub_tcp_each_server/ub_tcp_each_server.c"
TCP_EACH_SERVER_BIN="$OUT_DIR/linqu_ub_tcp_each_server"
UDMA_SRC="$ROOT_DIR/apps/ub_udma/ub_udma.c"
UDMA_BIN="$OUT_DIR/linqu_ub_udma"
OBMM_POOL_SRC="$ROOT_DIR/apps/ub_obmm_pool/ub_obmm_pool.c"
OBMM_POOL_BIN="$OUT_DIR/linqu_ub_obmm_pool"
OBMM_QUEUE_SRC="$ROOT_DIR/apps/obmm_queue/obmm_queue.c"
OBMM_QUEUE_BIN="$OUT_DIR/linqu_ub_obmm_queue"
OBMM_IMPORT_STRESS_SRC="$ROOT_DIR/apps/obmm_import_stress/obmm_import_stress.c"
OBMM_IMPORT_STRESS_BIN="$OUT_DIR/linqu_ub_obmm_import_stress"
OBMM_DATAPLANE_MICROBENCH_SRC="$ROOT_DIR/apps/obmm_dataplane_microbench/obmm_dataplane_microbench.c"
OBMM_DATAPLANE_MICROBENCH_BIN="$OUT_DIR/linqu_ub_obmm_dataplane_microbench"
GVA_DIRECT_SRC="$ROOT_DIR/apps/gva_direct/gva_direct.c"
GVA_DIRECT_BIN="$OUT_DIR/linqu_gva_direct"
OBMM_GSVA_SRC="$ROOT_DIR/apps/obmm_gsva/obmm_gsva.c"
OBMM_GSVA_BIN="$OUT_DIR/linqu_ub_obmm_gsva"
GVA_MANAGER_SRC="$ROOT_DIR/apps/gva_manager/gva_manager.c"
GVA_MANAGER_BIN="$OUT_DIR/linqu_gva_manager"
OBMM_COH_TEST_SRC="$ROOT_DIR/apps/obmm_coh_test/obmm_coh_test.c"
OBMM_COH_TEST_BIN="$OUT_DIR/linqu_ub_obmm_coh_test"
GSVA_QUERY_SRC="$ROOT_DIR/apps/gsva_query/gsva_query.c"
GSVA_QUERY_BIN="$OUT_DIR/linqu_ub_gsva_query"
GSVA_COH_TEST_SRC="$ROOT_DIR/apps/gsva_coh_test/gsva_coh_test.c"
GSVA_COH_TEST_BIN="$OUT_DIR/linqu_ub_gsva_coh_test"
GSVA_LIFECYCLE_TEST_SRC="$ROOT_DIR/apps/gsva_lifecycle_test/gsva_lifecycle_test.c"
GSVA_LIFECYCLE_TEST_BIN="$OUT_DIR/linqu_ub_gsva_lifecycle_test"
NPU_TEST_SRC="$ROOT_DIR/apps/npu_test/npu_test.c"
NPU_TEST_BIN="$OUT_DIR/npu_test"
SSD_TEST_SRC="$ROOT_DIR/apps/ssd_test/ssd_test.c"
SSD_TEST_BIN="$OUT_DIR/ssd_test"
NPU_GSVA_TEST_SRC="$ROOT_DIR/apps/npu_gsva_test/npu_gsva_test.c"
NPU_GSVA_TEST_BIN="$OUT_DIR/npu_gsva_test"
SSD_GSVA_TEST_SRC="$ROOT_DIR/apps/ssd_gsva_test/ssd_gsva_test.c"
SSD_GSVA_TEST_BIN="$OUT_DIR/ssd_gsva_test"
MEM_SERVICE_CLI_SRC="$ROOT_DIR/apps/mem_service/mem_service.c"
LLM_INFER_APP_SRC="$ROOT_DIR/apps/llm_infer/llm_infer.c"
MEM_SERVICE_SRC="$ROOT_DIR/components/mem_service/mem_service.c"
MEM_SERVICE_CLUSTER_UTILS_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_utils.c"
MEM_SERVICE_CLUSTER_PAYLOAD_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_payload.c"
MEM_SERVICE_CLUSTER_READ_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_read.c"
MEM_SERVICE_CLUSTER_RUNTIME_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_runtime.c"
MEM_SERVICE_CLUSTER_QUEUE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_queue.c"
MEM_SERVICE_CLUSTER_OBSERVE_SRC="$ROOT_DIR/components/mem_service/mem_service_cluster_observe.c"
MEM_SERVICE_OBMM_OBJECT_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_object_flow.c"
MEM_SERVICE_METADATA_SRC="$ROOT_DIR/components/mem_service/mem_service_metadata.c"
MEM_SERVICE_KEYS_SRC="$ROOT_DIR/components/mem_service/mem_service_keys.c"
MEM_SERVICE_OBJECT_REFS_SRC="$ROOT_DIR/components/mem_service/mem_service_object_refs.c"
MEM_SERVICE_OBMM_OBJECTS_SRC="$ROOT_DIR/components/mem_service/mem_service_obmm_objects.c"
MEM_SERVICE_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_records.c"
MEM_SERVICE_QWEN3_RECORDS_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_records.c"
MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_decode_barrier.c"
MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_kv_state_flow.c"
MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_terminal_token_flow.c"
MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c"
MEM_SERVICE_QWEN3_SRC="$ROOT_DIR/components/mem_service/mem_service_qwen3.c"
LLM_INFER_SRC="$ROOT_DIR/components/llm_infer/llm_infer.c"
MEM_SERVICE_CLI_BIN="$OUT_DIR/linqu_mem_service"
LLM_INFER_APP_BIN="$OUT_DIR/linqu_llm_infer"
RUN_APP_SRC="$ROOT_DIR/initramfs/run_app"
RUN_APP_BIN="$INITRAMFS_DIR/bin/run_app"
INIT_SCRIPT_SRC="$ROOT_DIR/initramfs/init"
INIT_SCRIPT_BIN="$INITRAMFS_DIR/init"
LINQU_INIT_BIN="$INITRAMFS_DIR/bin/linqu_init"
RDINIT_INTERACTIVE_SRC="$ROOT_DIR/initramfs/rdinit_interactive"
RDINIT_INTERACTIVE_BIN="$INITRAMFS_DIR/bin/rdinit_interactive"
INIT_BIN_TO_USE="${INIT_TO_USE:-$INIT_BIN}"
INITRAMFS_IMG="$OUT_DIR/initramfs.cpio.gz"
INITRAMFS_STAMP_FILE="$OUT_DIR/.initramfs.inputs.stamp"
KERNEL_STAMP_FILE="$OUT_DIR/.kernel_image.kernel_ub_head"

LINQU_MODULE="${LINQU_UB_GUEST_MODULE:-}"
HISI_UBUS_MODULE="${HISI_UBUS_GUEST_MODULE:-}"
UBUS_MODULE="${UB_UBUS_GUEST_MODULE:-}"
UB_SIM_DECODER_MODULE="${UB_SIM_DECODER_GUEST_MODULE:-}"
OBMM_MODULE="${UB_OBMM_GUEST_MODULE:-}"
UBASE_MODULE="${UB_UBASE_GUEST_MODULE:-}"
UBCORE_MODULE="${UB_UBCORE_GUEST_MODULE:-}"
UDMA_MODULE="${UB_UDMA_GUEST_MODULE:-}"
IPOURMA_MODULE="${UB_IPOURMA_GUEST_MODULE:-}"
UBURMA_MODULE="${UB_URMA_GUEST_MODULE:-}"
UMMU_CORE_MODULE="${UB_UMMU_CORE_GUEST_MODULE:-}"
UMMU_MODULE="${UB_UMMU_GUEST_MODULE:-}"

COPY_ALL_KO="${COPY_ALL_KO:-0}"
ALLOW_OUT_DIR_MODULE_FALLBACK="${ALLOW_OUT_DIR_MODULE_FALLBACK:-0}"

: "${AARCH64_LINUX_CC:=}"
: "${BUSYBOX:=}"
if [[ -z "$BUSYBOX" ]] && [[ -x "$ROOT_DIR/busybox-aarch64" ]]; then
  BUSYBOX="$ROOT_DIR/busybox-aarch64"
fi

detect_make_jobs() {
  getconf _NPROCESSORS_ONLN 2>/dev/null || echo 8
}

hash_file() {
  local path="$1"
  if [[ -x /usr/bin/shasum ]]; then
    /usr/bin/shasum -a 256 "$path" | /usr/bin/awk '{print $1}'
    return 0
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$path" | awk '{print $1}'
    return 0
  fi
  sha256sum "$path" | awk '{print $1}'
}

write_signature_line() {
  local label="$1"
  local path="$2"
  if [[ -e "$path" ]]; then
    printf '%s=%s:%s\n' "$label" "$path" "$(hash_file "$path")"
  else
    printf '%s=%s:MISSING\n' "$label" "$path"
  fi
}

current_kernel_signature() {
  if [[ -f "$KERNEL_STAMP_FILE" ]]; then
    cat "$KERNEL_STAMP_FILE"
    return 0
  fi
  git -C "$ROOT_DIR/../kernel_ub" rev-parse HEAD 2>/dev/null || echo ""
}

current_initramfs_signature() {
  local applet=""
  printf 'kernel_head=%s\n' "$(current_kernel_signature)"
  printf 'cc=%s\n' "$AARCH64_LINUX_CC"
  write_signature_line "build_initramfs_script" "$SCRIPT_DIR/build_initramfs.sh"
  write_signature_line "busybox" "$BUSYBOX"
  write_signature_line "probe_src" "$PROBE_SRC"
  write_signature_line "urma_dp_src" "$URMA_DP_SRC"
  write_signature_line "init_src" "$INIT_SRC"
  write_signature_line "insmod_src" "$INSMOD_SRC"
  write_signature_line "init_manual_bind_src" "$INIT_MANUAL_BIND_SRC"
  write_signature_line "chat_src" "$CHAT_SRC"
  write_signature_line "rpc_src" "$RPC_SRC"
  write_signature_line "tcp_each_server_src" "$TCP_EACH_SERVER_SRC"
  write_signature_line "udma_src" "$UDMA_SRC"
  write_signature_line "obmm_pool_src" "$OBMM_POOL_SRC"
  write_signature_line "obmm_queue_src" "$OBMM_QUEUE_SRC"
  write_signature_line "obmm_import_stress_src" "$OBMM_IMPORT_STRESS_SRC"
  write_signature_line "obmm_dataplane_microbench_src" "$OBMM_DATAPLANE_MICROBENCH_SRC"
  write_signature_line "gva_direct_src" "$GVA_DIRECT_SRC"
  write_signature_line "obmm_coh_test_src" "$OBMM_COH_TEST_SRC"
  write_signature_line "obmm_gsva_src" "$OBMM_GSVA_SRC"
  write_signature_line "gva_manager_src" "$GVA_MANAGER_SRC"
  write_signature_line "gsva_query_src" "$GSVA_QUERY_SRC"
  write_signature_line "gsva_coh_test_src" "$GSVA_COH_TEST_SRC"
  write_signature_line "gsva_lifecycle_test_src" "$GSVA_LIFECYCLE_TEST_SRC"
  write_signature_line "npu_test_src" "$NPU_TEST_SRC"
  write_signature_line "ssd_test_src" "$SSD_TEST_SRC"
  write_signature_line "npu_gsva_test_src" "$NPU_GSVA_TEST_SRC"
  write_signature_line "ssd_gsva_test_src" "$SSD_GSVA_TEST_SRC"
  write_signature_line "mem_service_cli_src" "$MEM_SERVICE_CLI_SRC"
  write_signature_line "llm_infer_app_src" "$LLM_INFER_APP_SRC"
  write_signature_line "mem_service_src" "$MEM_SERVICE_SRC"
  write_signature_line "mem_service_cluster_utils_src" "$MEM_SERVICE_CLUSTER_UTILS_SRC"
  write_signature_line "mem_service_cluster_payload_src" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC"
  write_signature_line "mem_service_cluster_read_src" "$MEM_SERVICE_CLUSTER_READ_SRC"
  write_signature_line "mem_service_cluster_runtime_src" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC"
  write_signature_line "mem_service_cluster_queue_src" "$MEM_SERVICE_CLUSTER_QUEUE_SRC"
  write_signature_line "mem_service_cluster_observe_src" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC"
  write_signature_line "mem_service_obmm_object_flow_src" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC"
  write_signature_line "mem_service_metadata_src" "$MEM_SERVICE_METADATA_SRC"
  write_signature_line "mem_service_keys_src" "$MEM_SERVICE_KEYS_SRC"
  write_signature_line "mem_service_object_refs_src" "$MEM_SERVICE_OBJECT_REFS_SRC"
  write_signature_line "mem_service_obmm_objects_src" "$MEM_SERVICE_OBMM_OBJECTS_SRC"
  write_signature_line "mem_service_records_src" "$MEM_SERVICE_RECORDS_SRC"
  write_signature_line "mem_service_qwen3_records_src" "$MEM_SERVICE_QWEN3_RECORDS_SRC"
  write_signature_line "mem_service_qwen3_decode_barrier_src" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC"
  write_signature_line "mem_service_qwen3_kv_state_flow_src" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC"
  write_signature_line "mem_service_qwen3_terminal_token_flow_src" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC"
  write_signature_line "mem_service_qwen3_runtime_range_publish_flow_src" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC"
  write_signature_line "mem_service_qwen3_src" "$MEM_SERVICE_QWEN3_SRC"
  write_signature_line "llm_infer_src" "$LLM_INFER_SRC"
  write_signature_line "run_app_src" "$RUN_APP_SRC"
  write_signature_line "init_script_src" "$INIT_SCRIPT_SRC"
  write_signature_line "rdinit_interactive_src" "$RDINIT_INTERACTIVE_SRC"
  for applet in "$ROOT_DIR"/*.h(N); do
    write_signature_line "header" "$applet"
  done
  for applet in "$OUT_DIR/modules"/*.ko(N) "$OUT_DIR"/*.ko(N); do
    write_signature_line "module" "$applet"
  done
}

initramfs_stamp_matches() {
  [[ -f "$INITRAMFS_STAMP_FILE" && -f "$INITRAMFS_IMG" ]] || return 1
  [[ "$(cat "$INITRAMFS_STAMP_FILE" 2>/dev/null)" == "$(current_initramfs_signature)" ]]
}

write_initramfs_stamp() {
  current_initramfs_signature > "$INITRAMFS_STAMP_FILE"
}

ensure_busybox_static_config() {
  local src_dir="$1"
  local cc_path="$2"
  local cc_name=""
  local cc_prefix=""

  cc_name="$(basename "$cc_path")"
  cc_prefix="${cc_name%gcc}"
  if [[ -z "$cc_prefix" || "$cc_prefix" == "$cc_name" ]]; then
    echo "[build_initramfs] error: cannot derive CROSS_COMPILER_PREFIX from $cc_path" >&2
    return 1
  fi

  make -C "$src_dir" defconfig >/dev/null

  perl -0pi -e 's/^# CONFIG_STATIC is not set$/CONFIG_STATIC=y/m' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_STATIC=.*$/CONFIG_STATIC=y/m' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_CROSS_COMPILER_PREFIX=.*\n//mg' "$src_dir/.config"
  perl -0pi -e 's/^CONFIG_EXTRA_CFLAGS=.*\n//mg' "$src_dir/.config"
  printf 'CONFIG_CROSS_COMPILER_PREFIX="%s"\n' "$cc_prefix" >> "$src_dir/.config"
  printf 'CONFIG_EXTRA_CFLAGS="-static"\n' >> "$src_dir/.config"
}

build_busybox_from_source_dir() {
  local src_dir="$1"
  local out_bin="$2"
  local cc_path="$3"
  local jobs

  jobs="$(detect_make_jobs)"
  echo "[build_initramfs] building busybox from source: $src_dir" >&2

  ensure_busybox_static_config "$src_dir" "$cc_path"
  make -C "$src_dir" -j"$jobs" >/dev/null

  if [[ ! -x "$src_dir/busybox" ]]; then
    echo "[build_initramfs] error: busybox build did not produce $src_dir/busybox" >&2
    return 1
  fi

  cp "$src_dir/busybox" "$out_bin"
  chmod +x "$out_bin"
}

ensure_busybox_binary() {
  local third_party_dir="$ROOT_DIR/third_party"
  local local_bin="$ROOT_DIR/busybox-aarch64"
  local third_party_bin="$third_party_dir/busybox-aarch64"
  local src_dir="$third_party_dir/busybox-src"
  local extracted_dir=""
  local tarball=""

  if [[ -n "$BUSYBOX" ]]; then
    if [[ ! -x "$BUSYBOX" ]]; then
      echo "[build_initramfs] error: BUSYBOX is set but not executable: $BUSYBOX" >&2
      return 1
    fi
    return 0
  fi

  if [[ -x "$local_bin" ]]; then
    BUSYBOX="$local_bin"
    return 0
  fi

  if [[ -x "$third_party_bin" ]]; then
    cp "$third_party_bin" "$local_bin"
    chmod +x "$local_bin"
    BUSYBOX="$local_bin"
    return 0
  fi

  if [[ -z "$AARCH64_LINUX_CC" ]]; then
    echo "[build_initramfs] error: AARCH64_LINUX_CC is required to build busybox" >&2
    return 1
  fi

  mkdir -p "$third_party_dir"

  if [[ -d "$src_dir" ]]; then
    build_busybox_from_source_dir "$src_dir" "$local_bin" "$AARCH64_LINUX_CC"
    BUSYBOX="$local_bin"
    return 0
  fi

  tarball="$(find "$third_party_dir" -maxdepth 1 -type f -name 'busybox-*.tar.bz2' | head -n 1)"
  if [[ -n "$tarball" ]]; then
    echo "[build_initramfs] extracting busybox source from $tarball" >&2
    tar -xf "$tarball" -C "$third_party_dir"
    extracted_dir="$(find "$third_party_dir" -maxdepth 1 -type d -name 'busybox-*' ! -name 'busybox-src' | head -n 1)"
    if [[ -z "$extracted_dir" ]]; then
      echo "[build_initramfs] error: failed to locate extracted busybox source under $third_party_dir" >&2
      return 1
    fi
    rm -rf "$src_dir"
    mv "$extracted_dir" "$src_dir"
    build_busybox_from_source_dir "$src_dir" "$local_bin" "$AARCH64_LINUX_CC"
    BUSYBOX="$local_bin"
    return 0
  fi

  echo "[build_initramfs] error: missing ARM64 busybox binary and no local busybox source/tarball available" >&2
  echo "[build_initramfs] expected one of:" >&2
  echo "[build_initramfs]   - BUSYBOX=/path/to/busybox-aarch64" >&2
  echo "[build_initramfs]   - $ROOT_DIR/busybox-aarch64" >&2
  echo "[build_initramfs]   - $third_party_bin" >&2
  echo "[build_initramfs]   - $src_dir" >&2
  echo "[build_initramfs]   - $third_party_dir/busybox-*.tar.bz2" >&2
  return 1
}

resolve_module_path() {
  local explicit_path="$1"
  local module_name="$2"
  local candidate=""

  if [[ -n "$explicit_path" ]]; then
    echo "$explicit_path"
    return 0
  fi

  candidate="$OUT_DIR/modules/$module_name"
  if [[ -f "$candidate" ]]; then
    echo "$candidate"
    return 0
  fi

  if [[ "$ALLOW_OUT_DIR_MODULE_FALLBACK" == "1" ]]; then
    candidate="$OUT_DIR/$module_name"
    if [[ -f "$candidate" ]]; then
      echo "$candidate"
      return 0
    fi
  fi

  echo ""
}

copy_module_if_present() {
  local src="$1"
  local dst_name="$2"
  local required="$3"
  local resolved=""

  resolved="$(resolve_module_path "$src" "$dst_name")"
  if [[ -z "$resolved" ]]; then
    if [[ "$required" == "1" ]]; then
      echo "[build_initramfs] warn: missing required module $dst_name" >&2
    fi
    return 0
  fi

  if [[ ! -f "$resolved" ]]; then
    echo "[build_initramfs] warn: module path not found: $resolved" >&2
    return 0
  fi

  cp "$resolved" "$INITRAMFS_DIR/lib/modules/$dst_name"
}

link_busybox_applet() {
  local applet="$1"
  ln -sf busybox "$INITRAMFS_DIR/bin/$applet"
}

mkdir -p "$OUT_DIR"
rm -rf "$INITRAMFS_DIR"
mkdir -p \
  "$INITRAMFS_DIR/bin" \
  "$INITRAMFS_DIR/dev" \
  "$INITRAMFS_DIR/proc" \
  "$INITRAMFS_DIR/sys" \
  "$INITRAMFS_DIR/tmp" \
  "$INITRAMFS_DIR/lib/modules"

if [[ -z "$AARCH64_LINUX_CC" ]]; then
  echo "AARCH64_LINUX_CC is required" >&2
  echo "example: export AARCH64_LINUX_CC=/path/to/aarch64-*-gnu-gcc" >&2
  exit 1
fi

ensure_busybox_binary

if initramfs_stamp_matches; then
  echo "[build_initramfs] initramfs is up to date: $INITRAMFS_IMG" >&2
  echo "$INITRAMFS_IMG"
  exit 0
fi

"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$PROBE_SRC" -o "$PROBE_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$URMA_DP_SRC" -o "$URMA_DP_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INIT_SRC" -o "$INIT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INSMOD_SRC" -o "$INSMOD_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$INIT_MANUAL_BIND_SRC" -o "$INIT_MANUAL_BIND_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$CHAT_SRC" -o "$CHAT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$RPC_SRC" -o "$RPC_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$TCP_EACH_SERVER_SRC" -o "$TCP_EACH_SERVER_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR" -I"$ROOT_DIR/.." "$UDMA_SRC" -o "$UDMA_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR" -I"$ROOT_DIR/.." "$OBMM_POOL_SRC" -o "$OBMM_POOL_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/libs/obmm_queue" -I"$ROOT_DIR/apps/obmm_queue" "$OBMM_QUEUE_SRC" -o "$OBMM_QUEUE_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/kernel_ub/include/uapi" -I"$ROOT_DIR/common" -I"$ROOT_DIR/apps/obmm_queue" "$OBMM_IMPORT_STRESS_SRC" -o "$OBMM_IMPORT_STRESS_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/kernel_ub/include/uapi" -I"$ROOT_DIR/common" "$OBMM_DATAPLANE_MICROBENCH_SRC" -o "$OBMM_DATAPLANE_MICROBENCH_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$GVA_DIRECT_SRC" -o "$GVA_DIRECT_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$OBMM_GSVA_SRC" -o "$OBMM_GSVA_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/kernel_ub/include/uapi" -I"$ROOT_DIR/common" -I"$ROOT_DIR/libs/obmm_queue" "$GVA_MANAGER_SRC" -o "$GVA_MANAGER_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$OBMM_COH_TEST_SRC" -o "$OBMM_COH_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/kernel_ub/include/uapi" -I"$ROOT_DIR/common" "$GSVA_QUERY_SRC" -o "$GSVA_QUERY_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$GSVA_COH_TEST_SRC" -o "$GSVA_COH_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$GSVA_LIFECYCLE_TEST_SRC" -o "$GSVA_LIFECYCLE_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$NPU_TEST_SRC" -o "$NPU_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra "$SSD_TEST_SRC" -o "$SSD_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$NPU_GSVA_TEST_SRC" -o "$NPU_GSVA_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR/common" "$SSD_GSVA_TEST_SRC" -o "$SSD_GSVA_TEST_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR" -I"$ROOT_DIR/.." -I"$ROOT_DIR/libs/obmm_queue" -I"$ROOT_DIR/apps/obmm_queue" "$MEM_SERVICE_CLI_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$MEM_SERVICE_CLI_BIN"
"$AARCH64_LINUX_CC" -static -O2 -Wall -Wextra -I"$ROOT_DIR" -I"$ROOT_DIR/.." -I"$ROOT_DIR/libs/obmm_queue" -I"$ROOT_DIR/apps/obmm_queue" "$LLM_INFER_APP_SRC" "$MEM_SERVICE_SRC" "$MEM_SERVICE_CLUSTER_UTILS_SRC" "$MEM_SERVICE_CLUSTER_PAYLOAD_SRC" "$MEM_SERVICE_CLUSTER_READ_SRC" "$MEM_SERVICE_CLUSTER_RUNTIME_SRC" "$MEM_SERVICE_CLUSTER_QUEUE_SRC" "$MEM_SERVICE_CLUSTER_OBSERVE_SRC" "$MEM_SERVICE_OBMM_OBJECT_FLOW_SRC" "$MEM_SERVICE_METADATA_SRC" "$MEM_SERVICE_KEYS_SRC" "$MEM_SERVICE_OBJECT_REFS_SRC" "$MEM_SERVICE_OBMM_OBJECTS_SRC" "$MEM_SERVICE_RECORDS_SRC" "$MEM_SERVICE_QWEN3_RECORDS_SRC" "$MEM_SERVICE_QWEN3_DECODE_BARRIER_SRC" "$MEM_SERVICE_QWEN3_KV_STATE_FLOW_SRC" "$MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW_SRC" "$MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW_SRC" "$MEM_SERVICE_QWEN3_SRC" "$LLM_INFER_SRC" -lm -o "$LLM_INFER_APP_BIN"

if [[ -f "$INIT_SCRIPT_SRC" ]]; then
  cp "$INIT_SCRIPT_SRC" "$INIT_SCRIPT_BIN"
  chmod +x "$INIT_SCRIPT_BIN"
else
  echo "[build_initramfs] error: missing init script template: $INIT_SCRIPT_SRC" >&2
  exit 1
fi

cp "$INIT_BIN_TO_USE" "$LINQU_INIT_BIN"
chmod +x "$LINQU_INIT_BIN"
cp "$PROBE_BIN" "$INITRAMFS_DIR/bin/linqu_probe"
cp "$URMA_DP_BIN" "$INITRAMFS_DIR/bin/linqu_urma_dp"
cp "$INSMOD_BIN" "$INITRAMFS_DIR/bin/insmod"
cp "$CHAT_BIN" "$INITRAMFS_DIR/bin/linqu_ub_chat"
cp "$RPC_BIN" "$INITRAMFS_DIR/bin/linqu_ub_rpc"
cp "$TCP_EACH_SERVER_BIN" "$INITRAMFS_DIR/bin/linqu_ub_tcp_each_server"
cp "$UDMA_BIN" "$INITRAMFS_DIR/bin/linqu_ub_udma"
cp "$OBMM_POOL_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_pool"
cp "$OBMM_QUEUE_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_queue"
cp "$OBMM_IMPORT_STRESS_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_import_stress"
cp "$OBMM_DATAPLANE_MICROBENCH_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_dataplane_microbench"
cp "$GVA_DIRECT_BIN" "$INITRAMFS_DIR/bin/linqu_gva_direct"
cp "$OBMM_GSVA_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_gsva"
cp "$GVA_MANAGER_BIN" "$INITRAMFS_DIR/bin/linqu_gva_manager"
cp "$OBMM_COH_TEST_BIN" "$INITRAMFS_DIR/bin/linqu_ub_obmm_coh_test"
cp "$GSVA_QUERY_BIN" "$INITRAMFS_DIR/bin/linqu_ub_gsva_query"
cp "$GSVA_COH_TEST_BIN" "$INITRAMFS_DIR/bin/linqu_ub_gsva_coh_test"
cp "$GSVA_LIFECYCLE_TEST_BIN" "$INITRAMFS_DIR/bin/linqu_ub_gsva_lifecycle_test"
cp "$NPU_TEST_BIN" "$INITRAMFS_DIR/bin/npu_test"
cp "$SSD_TEST_BIN" "$INITRAMFS_DIR/bin/ssd_test"
cp "$NPU_GSVA_TEST_BIN" "$INITRAMFS_DIR/bin/npu_gsva_test"
cp "$SSD_GSVA_TEST_BIN" "$INITRAMFS_DIR/bin/ssd_gsva_test"
cp "$MEM_SERVICE_CLI_BIN" "$INITRAMFS_DIR/bin/linqu_mem_service"
cp "$LLM_INFER_APP_BIN" "$INITRAMFS_DIR/bin/linqu_llm_infer"
chmod +x \
  "$INITRAMFS_DIR/bin/linqu_probe" \
  "$INITRAMFS_DIR/bin/linqu_urma_dp" \
  "$INITRAMFS_DIR/bin/insmod" \
  "$INITRAMFS_DIR/bin/linqu_ub_chat" \
  "$INITRAMFS_DIR/bin/linqu_ub_rpc" \
  "$INITRAMFS_DIR/bin/linqu_ub_tcp_each_server" \
  "$INITRAMFS_DIR/bin/linqu_ub_udma" \
  "$INITRAMFS_DIR/bin/linqu_ub_obmm_pool" \
  "$INITRAMFS_DIR/bin/linqu_ub_obmm_queue" \
  "$INITRAMFS_DIR/bin/linqu_ub_obmm_dataplane_microbench" \
  "$INITRAMFS_DIR/bin/linqu_gva_direct" \
  "$INITRAMFS_DIR/bin/linqu_ub_obmm_gsva" \
  "$INITRAMFS_DIR/bin/linqu_gva_manager" \
  "$INITRAMFS_DIR/bin/linqu_ub_obmm_coh_test" \
  "$INITRAMFS_DIR/bin/linqu_ub_gsva_query" \
  "$INITRAMFS_DIR/bin/linqu_ub_gsva_coh_test" \
  "$INITRAMFS_DIR/bin/linqu_ub_gsva_lifecycle_test" \
  "$INITRAMFS_DIR/bin/linqu_mem_service" \
  "$INITRAMFS_DIR/bin/linqu_llm_infer" \
  "$INITRAMFS_DIR/bin/npu_test" \
  "$INITRAMFS_DIR/bin/ssd_test" \
  "$INITRAMFS_DIR/bin/npu_gsva_test" \
  "$INITRAMFS_DIR/bin/ssd_gsva_test"

cp "$BUSYBOX" "$INITRAMFS_DIR/bin/busybox"
chmod +x "$INITRAMFS_DIR/bin/busybox"
link_busybox_applet sh
link_busybox_applet ls
link_busybox_applet mount
link_busybox_applet mkdir
link_busybox_applet cat
link_busybox_applet sleep
link_busybox_applet dmesg
link_busybox_applet head
link_busybox_applet tail
link_busybox_applet grep
link_busybox_applet ps
link_busybox_applet uname
link_busybox_applet ifconfig
link_busybox_applet route
link_busybox_applet netstat
link_busybox_applet ip
link_busybox_applet arp
link_busybox_applet ping
link_busybox_applet ping6

if [[ -f "$RUN_APP_SRC" ]]; then
  cp "$RUN_APP_SRC" "$RUN_APP_BIN"
  chmod +x "$RUN_APP_BIN"
else
  echo "[build_initramfs] warn: missing run_app script template: $RUN_APP_SRC" >&2
fi

if [[ -f "$RDINIT_INTERACTIVE_SRC" ]]; then
  cp "$RDINIT_INTERACTIVE_SRC" "$RDINIT_INTERACTIVE_BIN"
  chmod +x "$RDINIT_INTERACTIVE_BIN"
else
  echo "[build_initramfs] warn: missing interactive rdinit template: $RDINIT_INTERACTIVE_SRC" >&2
fi

if [[ "$COPY_ALL_KO" == "1" ]] && [[ -d "$OUT_DIR" ]]; then
  for ko_file in "$OUT_DIR"/*.ko; do
    if [[ -f "$ko_file" ]]; then
      cp "$ko_file" "$INITRAMFS_DIR/lib/modules/"
    fi
  done
fi

copy_module_if_present "$LINQU_MODULE" "linqu_ub_drv.ko" 0
copy_module_if_present "$HISI_UBUS_MODULE" "hisi_ubus.ko" 0
copy_module_if_present "$UBUS_MODULE" "ubus.ko" 0
copy_module_if_present "$UB_SIM_DECODER_MODULE" "ub-sim-decoder.ko" 0
copy_module_if_present "$OBMM_MODULE" "obmm.ko" 0
copy_module_if_present "$UBASE_MODULE" "ubase.ko" 0
copy_module_if_present "$UBCORE_MODULE" "ubcore.ko" 0
copy_module_if_present "$UDMA_MODULE" "udma.ko" 0
copy_module_if_present "$IPOURMA_MODULE" "ipourma.ko" 0
copy_module_if_present "$UBURMA_MODULE" "uburma.ko" 0
copy_module_if_present "$UMMU_CORE_MODULE" "ummu-core.ko" 0
copy_module_if_present "$UMMU_MODULE" "ummu.ko" 0

echo "[build_initramfs] packaged modules:"
ls -1 "$INITRAMFS_DIR/lib/modules" | sed 's/^/[build_initramfs]   /'

(
  cd "$INITRAMFS_DIR"
  printf 'console\0' | cpio -o --null -H newc --quiet > /dev/null 2>&1 || true
)

(
  cd "$INITRAMFS_DIR"
  find . -print | cpio -o -H newc --quiet | gzip -9 > "$INITRAMFS_IMG"
)

write_initramfs_stamp

echo "$INITRAMFS_IMG"
echo "built $INITRAMFS_IMG"
