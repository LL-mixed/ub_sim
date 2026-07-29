CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
# mem_service lives in the standalone mem_service repository (sibling checkout
# by default). Override MEM_SERVICE_ROOT to point at another checkout.
MEM_SERVICE_ROOT ?= $(abspath $(ROOT)/../../../mem_service)
MEM_SERVICE := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_module.c
MEM_SERVICE_CLUSTER_UTILS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_utils.c
MEM_SERVICE_CLUSTER_PAYLOAD := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_payload.c
MEM_SERVICE_CLUSTER_READ := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_read.c
MEM_SERVICE_CLUSTER_RUNTIME := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_runtime.c
MEM_SERVICE_CLUSTER_QUEUE := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_queue.c
MEM_SERVICE_CLUSTER_OBSERVE := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_cluster_observe.c
MEM_SERVICE_OBMM_OBJECT_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_obmm_object_flow.c
MEM_SERVICE_CLIENT := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_client.c
MEM_SERVICE_WIRE_CLIENT := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_wire_client.c
MEM_SERVICE_METADATA := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_metadata.c
MEM_SERVICE_PROVIDER := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_provider.c
MEM_SERVICE_KEYS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_keys.c
MEM_SERVICE_OBJECT_REFS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_object_refs.c
MEM_SERVICE_OBMM_OBJECTS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_obmm_objects.c
MEM_SERVICE_UB_SSD_GSVA_BACKEND := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_ub_ssd_gsva_backend.c
MEM_SERVICE_UB_SSD_GSVA_IO := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_ub_ssd_gsva_io.c
MEM_SERVICE_RECORDS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_records.c
MEM_SERVICE_PROFILE := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_profile.c
MEM_SERVICE_DEEPSEEK_V4_FLASH := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_deepseek_v4_flash.c
MEM_SERVICE_EXPERT_ROUTE_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_expert_route_flow.c
MEM_SERVICE_EXPERT_CACHE := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_expert_cache.c
MEM_SERVICE_QWEN3_RECORDS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_records.c
MEM_SERVICE_QWEN3_RUNTIME := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_runtime.c
MEM_SERVICE_QWEN3_DECODE_BARRIER := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_decode_barrier.c
MEM_SERVICE_QWEN3_KV_STATE_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_kv_state_flow.c
MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_terminal_token_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_engram_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3_engram_wait_flow.c
MEM_SERVICE_QWEN3 := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_qwen3.c
LLM_INFER := $(ROOT)/components/llm_infer/llm_infer.c

all: linqu_llm_infer

linqu_llm_infer: llm_infer.c $(MEM_SERVICE) $(MEM_SERVICE_CLUSTER_UTILS) $(MEM_SERVICE_CLUSTER_PAYLOAD) $(MEM_SERVICE_CLUSTER_READ) $(MEM_SERVICE_CLUSTER_RUNTIME) $(MEM_SERVICE_CLUSTER_QUEUE) $(MEM_SERVICE_CLUSTER_OBSERVE) $(MEM_SERVICE_OBMM_OBJECT_FLOW) $(MEM_SERVICE_CLIENT) $(MEM_SERVICE_WIRE_CLIENT) $(MEM_SERVICE_METADATA) $(MEM_SERVICE_PROVIDER) $(MEM_SERVICE_KEYS) $(MEM_SERVICE_OBJECT_REFS) $(MEM_SERVICE_OBMM_OBJECTS) $(MEM_SERVICE_UB_SSD_GSVA_BACKEND) $(MEM_SERVICE_UB_SSD_GSVA_IO) $(MEM_SERVICE_RECORDS) $(MEM_SERVICE_PROFILE) $(MEM_SERVICE_DEEPSEEK_V4_FLASH) $(MEM_SERVICE_EXPERT_ROUTE_FLOW) $(MEM_SERVICE_EXPERT_CACHE) $(MEM_SERVICE_QWEN3_RECORDS) $(MEM_SERVICE_QWEN3_RUNTIME) $(MEM_SERVICE_QWEN3_DECODE_BARRIER) $(MEM_SERVICE_QWEN3_KV_STATE_FLOW) $(MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW) $(MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW) $(MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW) $(MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW) $(MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW) $(MEM_SERVICE_QWEN3) $(LLM_INFER)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue -I$(MEM_SERVICE_ROOT) -I$(MEM_SERVICE_ROOT)/libs/obmm_queue $^ -lm -o $@

clean:
	rm -f linqu_llm_infer

.PHONY: all clean
