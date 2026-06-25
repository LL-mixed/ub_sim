ROOT := ../..
CC = aarch64-linux-gnu-gcc
CFLAGS ?= -O2 -Wall -Wextra -static
INCLUDES := -I$(ROOT) -I$(ROOT)/.. -I$(ROOT)/libs/obmm_queue -I$(ROOT)/apps/obmm_queue

MEM_SERVICE := $(ROOT)/components/mem_service/mem_service.c
MEM_SERVICE_CLUSTER_UTILS := $(ROOT)/components/mem_service/mem_service_cluster_utils.c
MEM_SERVICE_CLUSTER_PAYLOAD := $(ROOT)/components/mem_service/mem_service_cluster_payload.c
MEM_SERVICE_CLUSTER_READ := $(ROOT)/components/mem_service/mem_service_cluster_read.c
MEM_SERVICE_CLUSTER_RUNTIME := $(ROOT)/components/mem_service/mem_service_cluster_runtime.c
MEM_SERVICE_CLUSTER_QUEUE := $(ROOT)/components/mem_service/mem_service_cluster_queue.c
MEM_SERVICE_CLUSTER_OBSERVE := $(ROOT)/components/mem_service/mem_service_cluster_observe.c
MEM_SERVICE_OBMM_OBJECT_FLOW := $(ROOT)/components/mem_service/mem_service_obmm_object_flow.c
MEM_SERVICE_METADATA := $(ROOT)/components/mem_service/mem_service_metadata.c
MEM_SERVICE_DAEMON := $(ROOT)/components/mem_service/mem_service_daemon.c
MEM_SERVICE_CLIENT := $(ROOT)/components/mem_service/mem_service_client.c
MEM_SERVICE_WIRE_CLIENT := $(ROOT)/components/mem_service/mem_service_wire_client.c
MEM_SERVICE_KEYS := $(ROOT)/components/mem_service/mem_service_keys.c
MEM_SERVICE_OBJECT_REFS := $(ROOT)/components/mem_service/mem_service_object_refs.c
MEM_SERVICE_OBMM_OBJECTS := $(ROOT)/components/mem_service/mem_service_obmm_objects.c
MEM_SERVICE_RECORDS := $(ROOT)/components/mem_service/mem_service_records.c
MEM_SERVICE_QWEN3_RECORDS := $(ROOT)/components/mem_service/mem_service_qwen3_records.c
MEM_SERVICE_QWEN3_RUNTIME := $(ROOT)/components/mem_service/mem_service_qwen3_runtime.c
MEM_SERVICE_QWEN3_DECODE_BARRIER := $(ROOT)/components/mem_service/mem_service_qwen3_decode_barrier.c
MEM_SERVICE_QWEN3_KV_STATE_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_kv_state_flow.c
MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_terminal_token_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_runtime_range_wait_flow.c
MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_runtime_range_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_engram_publish_flow.c
MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW := $(ROOT)/components/mem_service/mem_service_qwen3_engram_wait_flow.c
MEM_SERVICE_QWEN3 := $(ROOT)/components/mem_service/mem_service_qwen3.c
LLM_INFER := $(ROOT)/components/llm_infer/llm_infer.c
MEM_SERVICE_RELEASE_MANIFEST := release-manifest.txt
MEM_SERVICE_WIRE_SCHEMA_MANIFEST := wire-schema.txt
MEM_SERVICE_CONFIG_SCHEMA := configs/mem_service.conf.schema
MEM_SERVICE_CONFIG_EXAMPLE := configs/mem_service.example.conf
MEM_SERVICE_DEPLOY_MANIFEST := deploy/linqu_mem_service.service

PREFIX ?= /usr/local
DESTDIR ?=
INSTALL_BINDIR := $(DESTDIR)$(PREFIX)/bin
INSTALL_INCLUDEDIR := $(DESTDIR)$(PREFIX)/include/lingqu/mem_service
INSTALL_SRCDIR := $(DESTDIR)$(PREFIX)/src/lingqu/mem_service
INSTALL_DATADIR := $(DESTDIR)$(PREFIX)/share/lingqu/mem_service
INSTALL_EXAMPLEDIR := $(INSTALL_DATADIR)/examples
INSTALL_CONFIGDIR := $(INSTALL_DATADIR)/config
INSTALL_DEPLOYDIR := $(INSTALL_DATADIR)/deploy

MEM_SERVICE_PUBLIC_HEADERS := \
	$(ROOT)/components/mem_service/mem_service.h \
	$(ROOT)/components/mem_service/mem_service_core.h \
	$(ROOT)/components/mem_service/mem_service_client.h \
	$(ROOT)/components/mem_service/mem_service_wire.h \
	$(ROOT)/components/mem_service/mem_service_wire_client.h \
	$(ROOT)/components/mem_service/mem_service_wire_payload.h \
	$(ROOT)/components/mem_service/mem_service_wire_schema.h \
	$(ROOT)/components/mem_service/lingqu_object_service.h

MEM_SERVICE_CLIENT_SDK_SRCS := \
	$(ROOT)/components/mem_service/mem_service_client.c \
	$(ROOT)/components/mem_service/mem_service_wire_client.c

MEM_SERVICE_CLIENT_EXAMPLES := \
	examples/mem_service_serving_example.c \
	examples/mem_service_pretraining_example.c

MEM_SERVICE_CORE_SRCS := \
	mem_service.c \
	$(MEM_SERVICE_DAEMON) \
	$(MEM_SERVICE_CLIENT) \
	$(MEM_SERVICE_WIRE_CLIENT) \
	$(MEM_SERVICE_METADATA) \
	$(MEM_SERVICE_KEYS) \
	$(MEM_SERVICE_OBJECT_REFS) \
	$(MEM_SERVICE_RECORDS)

MEM_SERVICE_QWEN3_ADAPTER_SRCS := \
	mem_service.c \
	$(MEM_SERVICE) \
	$(MEM_SERVICE_CLUSTER_UTILS) \
	$(MEM_SERVICE_CLUSTER_PAYLOAD) \
	$(MEM_SERVICE_CLUSTER_READ) \
	$(MEM_SERVICE_CLUSTER_RUNTIME) \
	$(MEM_SERVICE_CLUSTER_QUEUE) \
	$(MEM_SERVICE_CLUSTER_OBSERVE) \
	$(MEM_SERVICE_OBMM_OBJECT_FLOW) \
	$(MEM_SERVICE_DAEMON) \
	$(MEM_SERVICE_CLIENT) \
	$(MEM_SERVICE_WIRE_CLIENT) \
	$(MEM_SERVICE_METADATA) \
	$(MEM_SERVICE_KEYS) \
	$(MEM_SERVICE_OBJECT_REFS) \
	$(MEM_SERVICE_OBMM_OBJECTS) \
	$(MEM_SERVICE_RECORDS) \
	$(MEM_SERVICE_QWEN3_RECORDS) \
	$(MEM_SERVICE_QWEN3_RUNTIME) \
	$(MEM_SERVICE_QWEN3_DECODE_BARRIER) \
	$(MEM_SERVICE_QWEN3_KV_STATE_FLOW) \
	$(MEM_SERVICE_QWEN3_TERMINAL_TOKEN_FLOW) \
	$(MEM_SERVICE_QWEN3_RUNTIME_RANGE_WAIT_FLOW) \
	$(MEM_SERVICE_QWEN3_RUNTIME_RANGE_PUBLISH_FLOW) \
	$(MEM_SERVICE_QWEN3_ENGRAM_PUBLISH_FLOW) \
	$(MEM_SERVICE_QWEN3_ENGRAM_WAIT_FLOW) \
	$(MEM_SERVICE_QWEN3) \
	$(LLM_INFER)

.PHONY: all install install-smoke print-release-manifest print-wire-schema clean

all: linqu_mem_service linqu_mem_service_core linqu_mem_service_qwen3

linqu_mem_service: $(MEM_SERVICE_CORE_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -lm -o $@

linqu_mem_service_core: $(MEM_SERVICE_CORE_SRCS)
	$(CC) $(CFLAGS) $(INCLUDES) $^ -lm -o $@

linqu_mem_service_qwen3: $(MEM_SERVICE_QWEN3_ADAPTER_SRCS)
	$(CC) $(CFLAGS) -DMEM_SERVICE_ENABLE_QWEN3_INSPECT $(INCLUDES) $^ -lm -o $@

print-release-manifest:
	@cat $(MEM_SERVICE_RELEASE_MANIFEST)

print-wire-schema:
	@cat $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST)

install: linqu_mem_service $(MEM_SERVICE_RELEASE_MANIFEST) $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST) $(MEM_SERVICE_CONFIG_SCHEMA) $(MEM_SERVICE_CONFIG_EXAMPLE) $(MEM_SERVICE_DEPLOY_MANIFEST)
	mkdir -p $(INSTALL_BINDIR) $(INSTALL_INCLUDEDIR) $(INSTALL_SRCDIR) $(INSTALL_DATADIR) $(INSTALL_EXAMPLEDIR) $(INSTALL_CONFIGDIR) $(INSTALL_DEPLOYDIR)
	cp linqu_mem_service $(INSTALL_BINDIR)/linqu_mem_service
	cp $(MEM_SERVICE_PUBLIC_HEADERS) $(INSTALL_INCLUDEDIR)/
	cp $(MEM_SERVICE_CLIENT_SDK_SRCS) $(INSTALL_SRCDIR)/
	cp $(MEM_SERVICE_CLIENT_EXAMPLES) $(INSTALL_EXAMPLEDIR)/
	cp $(MEM_SERVICE_RELEASE_MANIFEST) $(INSTALL_DATADIR)/release-manifest.txt
	cp $(MEM_SERVICE_WIRE_SCHEMA_MANIFEST) $(INSTALL_DATADIR)/wire-schema.txt
	cp $(MEM_SERVICE_CONFIG_SCHEMA) $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	cp $(MEM_SERVICE_CONFIG_EXAMPLE) $(INSTALL_CONFIGDIR)/mem_service.example.conf
	cp $(MEM_SERVICE_DEPLOY_MANIFEST) $(INSTALL_DEPLOYDIR)/linqu_mem_service.service

install-smoke: install
	test -x $(INSTALL_BINDIR)/linqu_mem_service
	test -f $(INSTALL_INCLUDEDIR)/mem_service_client.h
	test -f $(INSTALL_INCLUDEDIR)/mem_service_wire.h
	test -f $(INSTALL_SRCDIR)/mem_service_client.c
	test -f $(INSTALL_SRCDIR)/mem_service_wire_client.c
	test -f $(INSTALL_EXAMPLEDIR)/mem_service_serving_example.c
	test -f $(INSTALL_EXAMPLEDIR)/mem_service_pretraining_example.c
	test -f $(INSTALL_DATADIR)/release-manifest.txt
	test -f $(INSTALL_DATADIR)/wire-schema.txt
	test -f $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	test -f $(INSTALL_CONFIGDIR)/mem_service.example.conf
	test -f $(INSTALL_DEPLOYDIR)/linqu_mem_service.service
	grep -q '^mem_service_release_manifest_version=1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^core_binary=bin/linqu_mem_service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_schema_manifest=share/lingqu/mem_service/wire-schema.txt$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^wire_schema_manifest_checksum=0x560b762f$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^config_schema=share/lingqu/mem_service/config/mem_service.conf.schema$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^config_example=share/lingqu/mem_service/config/mem_service.example.conf$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^deployment_manifest=share/lingqu/mem_service/deploy/linqu_mem_service.service$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^metrics_export_format=prometheus-text$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_retry_policy=explicit-max-attempts-backoff$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_api=pretraining-refs-v1$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^public_header=include/lingqu/mem_service/mem_service_client.h$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^client_source=src/lingqu/mem_service/mem_service_client.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^example_source=share/lingqu/mem_service/examples/mem_service_serving_example.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^example_source=share/lingqu/mem_service/examples/mem_service_pretraining_example.c$$' $(INSTALL_DATADIR)/release-manifest.txt
	grep -q '^mem_service_wire_schema_manifest_version=1$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^operation_count=22$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^field_count=100$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^oneof_field=resolve_kv_segment.0.block_hash$$' $(INSTALL_DATADIR)/wire-schema.txt
	grep -q '^mem_service_config_schema_version=1$$' $(INSTALL_CONFIGDIR)/mem_service.conf.schema
	grep -q '^listen=unix:/tmp/linqu_mem_service.sock$$' $(INSTALL_CONFIGDIR)/mem_service.example.conf
	grep -q '^ExecStart=/usr/bin/linqu_mem_service serve --config /etc/lingqu/mem_service/mem_service.conf$$' $(INSTALL_DEPLOYDIR)/linqu_mem_service.service

clean:
	rm -f linqu_mem_service linqu_mem_service_core linqu_mem_service_qwen3
