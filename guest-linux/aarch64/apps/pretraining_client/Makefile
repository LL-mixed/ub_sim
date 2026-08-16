CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
ROOT := ../..
# mem_service lives in the root-level Git submodule. Override MEM_SERVICE_ROOT
# only when intentionally validating another standalone checkout.
MEM_SERVICE_ROOT ?= $(abspath $(ROOT)/../../mem_service)
MEM_SERVICE_CLIENT := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_client.c
MEM_SERVICE_WIRE_CLIENT := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_wire_client.c
MEM_SERVICE_KEYS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_keys.c
MEM_SERVICE_OBJECT_REFS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_object_refs.c
MEM_SERVICE_RECORDS := $(MEM_SERVICE_ROOT)/components/mem_service/mem_service_records.c

all: linqu_pretraining_client

linqu_pretraining_client: pretraining_client.c $(MEM_SERVICE_CLIENT) $(MEM_SERVICE_WIRE_CLIENT) $(MEM_SERVICE_KEYS) $(MEM_SERVICE_OBJECT_REFS) $(MEM_SERVICE_RECORDS)
	$(CC) $(CFLAGS) -I$(ROOT) -I$(ROOT)/.. -I$(MEM_SERVICE_ROOT) -I$(MEM_SERVICE_ROOT)/libs/obmm_queue $^ -lm -o $@

clean:
	rm -f linqu_pretraining_client

.PHONY: all clean
