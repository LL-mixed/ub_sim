CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
MEM_SERVICE := $(ROOT)/components/mem_service/mem_service.c
MEM_SERVICE_CLUSTER_UTILS := $(ROOT)/components/mem_service/mem_service_cluster_utils.c
MEM_SERVICE_KEYS := $(ROOT)/components/mem_service/mem_service_keys.c
MEM_SERVICE_OBJECT_REFS := $(ROOT)/components/mem_service/mem_service_object_refs.c
MEM_SERVICE_OBMM_OBJECTS := $(ROOT)/components/mem_service/mem_service_obmm_objects.c
MEM_SERVICE_RECORDS := $(ROOT)/components/mem_service/mem_service_records.c
MEM_SERVICE_QWEN3 := $(ROOT)/components/mem_service/mem_service_qwen3.c
LLM_INFER := $(ROOT)/components/llm_infer/llm_infer.c

all: linqu_llm_infer

linqu_llm_infer: llm_infer.c $(MEM_SERVICE) $(MEM_SERVICE_CLUSTER_UTILS) $(MEM_SERVICE_KEYS) $(MEM_SERVICE_OBJECT_REFS) $(MEM_SERVICE_OBMM_OBJECTS) $(MEM_SERVICE_RECORDS) $(MEM_SERVICE_QWEN3) $(LLM_INFER)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue $^ -lm -o $@

clean:
	rm -f linqu_llm_infer

.PHONY: all clean
